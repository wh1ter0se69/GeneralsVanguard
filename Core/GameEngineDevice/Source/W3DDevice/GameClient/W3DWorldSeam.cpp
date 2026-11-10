// GeneralsX: THE WORLD/HUD SEAM - see W3DWorldSeam.h and
// modernize/render-dlss/DESIGN_hud-seam.md §2 (mechanics), §3 (what follows the smaller target), §5 (IGxWorldPass), §7 (exactness).

#include "Lib/BaseType.h"
#include "WWLib/always.h"
#include "W3DDevice/GameClient/W3DWorldSeam.h"

#include "Common/GlobalData.h"
#include "GameClient/GXGraphicsSettings.h"
#include "GameClient/Water.h"
#include "WW3D2/camera.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/Renderer.h"
#include "WW3D2/ww3d.h"
#include "W3DDevice/GameClient/GxWorldPass.h"
#include "W3DDevice/GameClient/GxSceneTap.h"   // D · look-fdepth-plus: F-depth+
#ifdef GX_RENDER_NATIVE
#include "W3DDevice/GameClient/GXRender/Scene/GxMotionUnits.h"   // D · render-motion-units-1 (A35): the object-velocity bridge
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
	struct Seam
	{
		// targets, keyed on (render size, sample count, depth format); D3DPOOL_DEFAULT, so release() drops them before Reset
		IDirect3DTexture8 *tex = nullptr;	// the world colour the upscale samples (single-sample)
		IDirect3DSurface8 *texSurf = nullptr;
		IDirect3DSurface8 *msaa = nullptr;	// the world renders here when the replaced target is multisampled
		IDirect3DSurface8 *depth = nullptr;
		int rw = 0, rh = 0;
		D3DMULTISAMPLE_TYPE ms = D3DMULTISAMPLE_NONE;
		D3DFORMAT colourFmt = D3DFMT_UNKNOWN;
		D3DFORMAT depthFmt = D3DFMT_UNKNOWN;
		uint32_t targetFlags = 0;	// the GXWP_TEMPORAL the targets were made for (a sampleable depth is an allocation-time property)

		// the device upscaler's output-size targets (IGxWorldPass::Upscale), keyed on (size, format); the seam composites outTex
		IDirect3DTexture8 *outTex = nullptr, *scrTex = nullptr;
		IDirect3DSurface8 *outSurf = nullptr, *scrSurf = nullptr;
		int outTexW = 0, outTexH = 0;
		D3DFORMAT outFmt = D3DFMT_UNKNOWN;
		bool outFailed = false;		// their allocation failed once: bilinear for the session (logged once)
		bool upFailed = false;		// the last frame's device upscale failed: the next world draws unbiased (it composites bilinearly)

		IGxWorldPass *wp = nullptr;	// the native device's interface; null on DXVK / GXCAP
		bool wpAsked = false;
		bool hookSet = false;
		bool failed = false;		// an allocation or bind failed: OFF for the session (logged once)
		bool resetHistory = true;	// targets (re)created: the temporal backends drop their history
		float lodBias = 0.0f;		// R-SL2: the texture mip LOD bias the world pass draws with (0 = none set)
		bool lodLeakLogged = false;

		// this frame
		bool active = false;
		IDirect3DSurface8 *savedRT = nullptr, *savedDS = nullptr;	// the wrapper's pair (a null RT = the back buffer)
		IDirect3DSurface8 *boundRT = nullptr;	// the device's bound colour at push: what restoreTarget() must find again
		int outW = 0, outH = 0;
		RECT src = {}, dst = {};	// the camera's viewport in the world target and in the replaced target (CameraClass::Apply's math)
		bool pillarbox = false;
		Int upscaler = 0;

		unsigned engaged = 0;	// frames the seam redirected (the picture gate's "engaged > 0")
		char lastState[256] = "";

		// D · look-fdepth-plus: F-depth+ (GxSceneTap.h) - asked once per device, whatever GXUpscaler says; null on DXVK / GXCAP
		IGxSceneTap *tap = nullptr;
		bool tapAsked = false;
		bool tapFrame = false;	// this tactical frame may still be tapped (armed at push, spent by opaqueDone, cleared at pop)
	};
	Seam s;

	// through TheSuperHackers' Renderer seam (render-seam-lint): the DX8 backend forwards it 1:1 (Backend/DX8Backend.cpp:90-100)
	void setViewport(const D3DVIEWPORT8 &v)
	{
		const RenderViewport r = { v.X, v.Y, v.Width, v.Height, v.MinZ, v.MaxZ };
		Renderer::Set_Viewport(r);
	}

	template <class T> void releaseRef(T *&p)
	{
		if (p != nullptr)
		{
			p->Release();
			p = nullptr;
		}
	}

	void logState(const char *state)
	{
		if (strcmp(state, s.lastState) == 0)
			return;
		strncpy(s.lastState, state, sizeof(s.lastState) - 1);
		fprintf(stderr, "[GXSEAM] %s engaged=%u\n", state, s.engaged);
	}

	void failOff(const char *what, HRESULT hr)
	{
		fprintf(stderr, "[GXSEAM] ERROR %s hresult=0x%08X - the seam is OFF for this session, the world draws as without it\n",
			what, (unsigned)hr);
		s.failed = true;
		W3DWorldSeam::release();
	}

	// R-SL2 (render-integration-5's ruling 13/10; the numbers dlss-streamline-1's, from NVIDIA / AMD / Intel's guides): the world's
	// textures at a reduced render scale take log2(render / output) of mip LOD bias, one stop more for the temporal upscalers that
	// rebuild detail from history (not XeSS: Intel's guide stops at log2); none for bilinear, none at 100%.
	float worldLodBias(Int up, int renderW, int outputW)
	{
		if (renderW >= outputW || up == GXGraphics::UPSCALER_BILINEAR)
			return 0.0f;
		const float b = log2f((float)renderW / (float)outputW);
		return GXGraphics::upscalerIsTemporal(up) && up != GXGraphics::UPSCALER_XESS ? b - 1.0f : b;
	}

	// Every stage through the wrapper's cached setter (a float in the DWORD). The game never sets MIPMAPLODBIAS otherwise
	// (dx8wrapper.cpp:5884 is commented out), so every stage holds the device default 0 outside the world pass. On MoltenVK the
	// native device clamps it to 0 (no samplerMipLodBias, GxSamplers.cpp:45-46; residual R29).
	void setLodBias(float bias)
	{
		DWORD v;
		memcpy(&v, &bias, sizeof(v));
		for (unsigned i = 0; i < 8; ++i)
			DX8Wrapper::Set_DX8_Texture_Stage_State(i, D3DTSS_MIPMAPLODBIAS, v);
		s.lodBias = bias;
	}

	// Test arms, read once: GX_SEAM_LODBIAS=off draws the scaled world with no bias (the A/B's other arm, same binary), =keep never
	// restores it after the world pass (the planted leak push()'s check must catch). Unset = the bias as above.
	char lodBiasMode()
	{
		static const char m = [] { const char *e = getenv("GX_SEAM_LODBIAS"); return e && (*e == 'o' || *e == 'k') ? *e : 'n'; }();
		return m;
	}

	// The seam's one sharpener (dlss-integrator's ruling 13/10) - test arms, read once: GX_SEAM_RCAS=off never runs it (the A/B's
	// other arm), =always runs it even after a backend that sharpens itself (proves the pass on a device with no such backend).
	char rcasMode()
	{
		static const char m = [] { const char *e = getenv("GX_SEAM_RCAS"); return e && (*e == 'o' || *e == 'a') ? *e : 'n'; }();
		return m;
	}

	bool ensureTargets(IDirect3DDevice8 *dev, int rw, int rh, D3DMULTISAMPLE_TYPE ms, D3DFORMAT colourFmt, D3DFORMAT depthFmt,
		uint32_t flags)
	{
		if (s.tex != nullptr && s.rw == rw && s.rh == rh && s.ms == ms && s.colourFmt == colourFmt && s.depthFmt == depthFmt &&
			s.targetFlags == (flags & GXWP_TEMPORAL))
			return true;
		releaseRef(s.texSurf);
		releaseRef(s.tex);
		releaseRef(s.msaa);
		releaseRef(s.depth);
		// The shapes Pillarbox_Setup already creates on both renderers (dx8wrapper.cpp:327-393), in the REPLACED target's formats
		// (a 16-bit or alpha-less back buffer blends differently): a render-target texture, an MSAA
		// surface when the replaced target is multisampled, and a depth of the replaced depth's format and sample count (stencil
		// included when that format has it: shadow volumes, stenciled player colour).
		if (s.wp != nullptr)
			s.wp->NextTargetsAreWorld((flags & GXWP_TEMPORAL) | (ms != D3DMULTISAMPLE_NONE ? (uint32_t)GXWP_MSAA_COLOUR : 0u));
		HRESULT hr = dev->CreateTexture(rw, rh, 1, D3DUSAGE_RENDERTARGET, colourFmt, D3DPOOL_DEFAULT, &s.tex);
		if (SUCCEEDED(hr))
			hr = s.tex->GetSurfaceLevel(0, &s.texSurf);
		if (SUCCEEDED(hr) && ms != D3DMULTISAMPLE_NONE)
			hr = dev->CreateRenderTarget(rw, rh, colourFmt, ms, FALSE, &s.msaa);
		if (SUCCEEDED(hr))
			hr = dev->CreateDepthStencilSurface(rw, rh, depthFmt, ms, &s.depth);
		if (FAILED(hr) || s.texSurf == nullptr)
		{
			if (s.wp != nullptr)
				s.wp->NextTargetsAreWorld(0);	// a half-made set must not arm the device's next unrelated target (motion-code-r1 P2 #7)
			failOff("world target allocation", hr);
			return false;
		}
		s.targetFlags = flags & GXWP_TEMPORAL;
		s.rw = rw;
		s.rh = rh;
		s.ms = ms;
		s.colourFmt = colourFmt;
		s.depthFmt = depthFmt;
		s.resetHistory = true;
		return true;
	}

	void releaseOutTargets()
	{
		releaseRef(s.outSurf);
		releaseRef(s.outTex);
		releaseRef(s.scrSurf);
		releaseRef(s.scrTex);
		s.outTexW = s.outTexH = 0;
	}

	// Two single-sample render-target textures of the output size for a device upscaler (FSR 1 needs both). A failure leaves the
	// frame on the bilinear quad; never the whole seam.
	bool ensureOutTargets(IDirect3DDevice8 *dev, int w, int h, D3DFORMAT fmt)
	{
		if (s.outFailed)
			return false;
		if (s.outTex != nullptr && s.outTexW == w && s.outTexH == h && s.outFmt == fmt)
			return true;
		releaseOutTargets();
		HRESULT hr = dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &s.outTex);
		if (SUCCEEDED(hr))
			hr = s.outTex->GetSurfaceLevel(0, &s.outSurf);
		if (SUCCEEDED(hr))
			hr = dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &s.scrTex);
		if (SUCCEEDED(hr))
			hr = s.scrTex->GetSurfaceLevel(0, &s.scrSurf);
		if (FAILED(hr) || s.outSurf == nullptr || s.scrSurf == nullptr)
		{
			fprintf(stderr, "[GXSEAM] ERROR upscaler output targets %dx%d hresult=0x%08X - bilinear for this session\n", w, h, (unsigned)hr);
			releaseOutTargets();
			s.outFailed = true;
			return false;
		}
		s.outTexW = w;
		s.outTexH = h;
		s.outFmt = fmt;
		return true;
	}

	// The camera's D3D viewport in a target of this size: CameraClass::Apply's own truncating math (camera.cpp:741-746).
	RECT viewportRect(const Vector2 &vmin, const Vector2 &vmax, int w, int h)
	{
		RECT r;
		r.left = (LONG)(unsigned int)(vmin.X * (float)w);
		r.top = (LONG)(unsigned int)(vmin.Y * (float)h);
		r.right = r.left + (LONG)(unsigned int)((vmax.X - vmin.X) * (float)w);
		r.bottom = r.top + (LONG)(unsigned int)((vmax.Y - vmin.Y) * (float)h);
		return r;
	}

	// The device state the composite changes, read back from the device before it and restored after it through the wrapper's
	// cached setters, so the HUD meets exactly the state today's frame leaves it (a blanket
	// Invalidate_Cached_Render_States leaves ZENABLE off for the 3D cursor) and the wrapper's cache matches the device.
	const D3DRENDERSTATETYPE kRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_LIGHTING, D3DRS_ALPHABLENDENABLE,
		D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_STENCILENABLE, D3DRS_COLORWRITEENABLE, D3DRS_CLIPPING };
	const D3DTEXTURESTAGESTATETYPE kT0[] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_TEXCOORDINDEX,
		D3DTSS_TEXTURETRANSFORMFLAGS, D3DTSS_ADDRESSU, D3DTSS_ADDRESSV, D3DTSS_MINFILTER, D3DTSS_MAGFILTER, D3DTSS_MIPFILTER };
	const D3DTEXTURESTAGESTATETYPE kT1[] = { D3DTSS_COLOROP, D3DTSS_ALPHAOP };
	const int kNRS = sizeof(kRS) / sizeof(kRS[0]), kNT0 = sizeof(kT0) / sizeof(kT0[0]), kNT1 = sizeof(kT1) / sizeof(kT1[0]);

	struct DeviceState
	{
		DWORD rs[sizeof(kRS) / sizeof(kRS[0])];
		DWORD t0[sizeof(kT0) / sizeof(kT0[0])];
		DWORD t1[sizeof(kT1) / sizeof(kT1[0])];
		IDirect3DBaseTexture8 *tex0 = nullptr;
		IDirect3DVertexBuffer8 *vb0 = nullptr;	// DrawPrimitiveUP unbinds stream 0
		UINT stride0 = 0;
		DWORD ps = 0, vs = 0;
		D3DVIEWPORT8 vp = {};
	};

	void saveState(IDirect3DDevice8 *dev, DeviceState &st)
	{
		for (int i = 0; i < kNRS; ++i)
			dev->GetRenderState(kRS[i], &st.rs[i]);
		for (int i = 0; i < kNT0; ++i)
			dev->GetTextureStageState(0, kT0[i], &st.t0[i]);
		for (int i = 0; i < kNT1; ++i)
			dev->GetTextureStageState(1, kT1[i], &st.t1[i]);
		dev->GetTexture(0, &st.tex0);	// AddRef'd
		dev->GetStreamSource(0, &st.vb0, &st.stride0);	// AddRef'd
		dev->GetPixelShader(&st.ps);
		dev->GetVertexShader(&st.vs);
		dev->GetViewport(&st.vp);
	}

	void restoreState(IDirect3DDevice8 *dev, DeviceState &st)
	{
		// the wrapper caches its stream-0 binding and would not re-send it for the HUD's next buffered draw: the native device
		// refuses a draw on an unbound stream (GxD3D8Translate.cpp:810). Raw, as the wrapper believes it is still bound.
		dev->SetStreamSource(0, st.vb0, st.stride0);
		releaseRef(st.vb0);
		for (int i = 0; i < kNRS; ++i)
			DX8Wrapper::Set_DX8_Render_State(kRS[i], st.rs[i]);
		for (int i = 0; i < kNT0; ++i)
			DX8Wrapper::Set_DX8_Texture_Stage_State(0, kT0[i], st.t0[i]);
		for (int i = 0; i < kNT1; ++i)
			DX8Wrapper::Set_DX8_Texture_Stage_State(1, kT1[i], st.t1[i]);
		DX8Wrapper::Set_DX8_Texture(0, st.tex0);
		releaseRef(st.tex0);
		DX8Wrapper::Set_Pixel_Shader(st.ps);
		DX8Wrapper::Set_Vertex_Shader(st.vs);
		setViewport(st.vp);
	}

	// One axis of the composite: output pixels o0..o1 show source texels s0..s1, the first pixel centre on the first texel centre:
	// pixel o0 + m samples s0 + 1/2 + m S/O. The world is drawn with D3D9's half-pixel convention in RENDER pixels, so the plain
	// centre-to-centre mapping put a scaled world 0.5 (O/S - 1) output px right and down of native (measured 13/10: bilinear 67%
	// +0.19..+0.28 px, predicted +0.247; NOTE_hud-seam-halfpixel-offset.md); this one cancels it and is the same mapping at 1:1.
	// The sample is clamped to the centre of the last texel on a source edge INSIDE the texture (CLAMP_TO_EDGE does it at the
	// texture's own edge): those pixels form a strip with a constant coordinate, the edge texel replicated, so no texel outside the
	// camera's viewport bleeds in and the scale stays exact; nothing samples below the first
	// texel centre. Segments end on PIXEL EDGES: an MSAA target shades a pixel a segment only partly covers at the pixel's centre,
	// extrapolated past that segment. Returns 1-3 segments, output [o0, o1) showing source t0 -> t1.
	struct Seg
	{
		int o0, o1;
		float t0, t1;
	};
	int axisSegments(int o0, int o1, int s0, int s1, int texSize, bool exact, Seg seg[3])
	{
		// Exact in integers: pixel o0 + m samples s0 + 1/2 + m S/O, so it is at or past s1 - 1/2 iff m S >= (S - 1) O.
		// A pixel ON a threshold joins a constant strip - the upper one, and pixel o0 itself (exactly s0 + 1/2) on a lower edge inside
		// the texture - so the sloped segment holds only pixels at least 1/O texel inside. push() guarantees S >= 1 and O >= S.
		if (exact)
		{
			seg[0] = { o0, o1, (float)s0, (float)s1 };
			return 1;
		}
		const long long S = s1 - s0, O = o1 - o0;
		const bool lo = s0 > 0, hi = s1 < texSize;
		int p = lo ? o0 + 1 : o0;
		int q = hi ? o0 + (int)(((S - 1) * O + S - 1) / S) : o1;	// the first pixel at or past the last texel centre
		p = p > o1 ? o1 : p;
		q = q < p ? p : (q > o1 ? o1 : q);
		auto at = [&](int e) { return (float)((double)s0 + 0.5 + ((double)(e - o0) - 0.5) * (double)S / (double)O); };	// at a pixel EDGE
		int n = 0;
		if (p > o0)
			seg[n++] = { o0, p, (float)s0 + 0.5f, (float)s0 + 0.5f };
		if (q > p)
			seg[n++] = { p, q, at(p), at(q) };
		if (o1 > q)
			seg[n++] = { q, o1, (float)s1 - 0.5f, (float)s1 - 0.5f };
		return n;
	}

	// XYZRHW quads from rect src of tex (texW x texH) onto the replaced target's viewport rect (s.dst): Pillarbox_End's blit
	// (dx8wrapper.cpp:500-530) plus the alpha channel, the stage-0 texcoord routing and clamped addressing. Outside s.dst the
	// replaced target keeps Begin_Render's clear, as today. POINT when src and s.dst have one size (an exact copy: the world at
	// 1:1, or a device upscaler's output: one quad); scaled, LINEAR over up to 3 x 3 quads (axisSegments), one draw.
	void drawQuad(IDirect3DDevice8 *dev, IDirect3DTexture8 *tex, int texW, int texH, const RECT &src)
	{
		DeviceState st;
		saveState(dev, st);

		const D3DVIEWPORT8 vp = { (DWORD)s.dst.left, (DWORD)s.dst.top, (DWORD)(s.dst.right - s.dst.left),
			(DWORD)(s.dst.bottom - s.dst.top), 0.0f, 1.0f };
		setViewport(vp);
		const DWORD set[] = { FALSE, FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, FALSE, FALSE, 0xF, FALSE };
		for (int i = 0; i < kNRS; ++i)
			DX8Wrapper::Set_DX8_Render_State(kRS[i], set[i]);
		const bool exact = (src.right - src.left) == (s.dst.right - s.dst.left) && (src.bottom - src.top) == (s.dst.bottom - s.dst.top);
		const DWORD filter = exact ? D3DTEXF_POINT : D3DTEXF_LINEAR;
		const DWORD set0[] = { D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTOP_SELECTARG1, D3DTA_TEXTURE, 0, D3DTTFF_DISABLE,
			D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, filter, filter, D3DTEXF_NONE };
		for (int i = 0; i < kNT0; ++i)
			DX8Wrapper::Set_DX8_Texture_Stage_State(0, kT0[i], set0[i]);
		for (int i = 0; i < kNT1; ++i)
			DX8Wrapper::Set_DX8_Texture_Stage_State(1, kT1[i], D3DTOP_DISABLE);
		DX8Wrapper::Set_DX8_Texture(0, tex);
		DX8Wrapper::Set_Pixel_Shader(0);
		DX8Wrapper::Set_Vertex_Shader(D3DFVF_XYZRHW | D3DFVF_TEX1);

		Seg sx[3], sy[3];
		const int nx = axisSegments(s.dst.left, s.dst.right, src.left, src.right, texW, exact, sx);
		const int ny = axisSegments(s.dst.top, s.dst.bottom, src.top, src.bottom, texH, exact, sy);
		// D3D8 half-pixel rule: XYZRHW corners at -0.5 put texel centres on pixel centres (as Pillarbox_End). Each quad splits on
		// the strip's own diagonal (top-right to bottom-left).
		struct BV { float x, y, z, rhw; float u, v; };
		BV tri[3 * 3 * 6];
		int n = 0;
		for (int j = 0; j < ny; ++j)
			for (int i = 0; i < nx; ++i)
			{
				const Seg &a = sx[i], &b = sy[j];
				const float x0 = (float)a.o0 - 0.5f, x1 = (float)a.o1 - 0.5f, y0 = (float)b.o0 - 0.5f, y1 = (float)b.o1 - 0.5f;
				const float u0 = a.t0 / (float)texW, u1 = a.t1 / (float)texW, v0 = b.t0 / (float)texH, v1 = b.t1 / (float)texH;
				const BV tl = { x0, y0, 0.0f, 1.0f, u0, v0 }, tr = { x1, y0, 0.0f, 1.0f, u1, v0 };
				const BV bl = { x0, y1, 0.0f, 1.0f, u0, v1 }, br = { x1, y1, 0.0f, 1.0f, u1, v1 };
				const BV quad[6] = { tl, tr, bl, tr, br, bl };
				for (const BV &v : quad)
					tri[n++] = v;
			}
		dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n / 3, tri, sizeof(BV));
		DX8Wrapper::Record_Direct_Draw();	// [GXGFX] dc= (graphics P2 i-02)

		restoreState(dev, st);
	}

	void dropSaved()
	{
		releaseRef(s.boundRT);
		releaseRef(s.savedRT);
		releaseRef(s.savedDS);
	}

	// Back to the replaced target and drop the saved pair. True only when the
	// device's bound colour AND depth are again the ones push() found bound (read back: the wrapper's back-buffer path reports no
	// result, and a device may switch the colour before refusing the depth). On a refused pillarbox pair the back buffer is bound
	// first, so the HUD never draws into, or the quad samples, the world target.
	bool restoreTarget(IDirect3DDevice8 *dev)
	{
		const bool set = DX8Wrapper::Set_Render_Target_Pair(s.savedRT, s.savedDS);
		if (!set && s.savedRT != nullptr)
			DX8Wrapper::Set_Render_Target((IDirect3DSurface8 *)nullptr);
		IDirect3DSurface8 *rt = nullptr, *ds = nullptr;
		const bool ok = set && SUCCEEDED(dev->GetRenderTarget(&rt)) && SUCCEEDED(dev->GetDepthStencilSurface(&ds)) &&
			rt == s.boundRT && ds == s.savedDS;
		releaseRef(rt);
		releaseRef(ds);
		dropSaved();
		return ok;
	}

	// F-depth+: this frame's world may be tapped once (opaqueDone) when the device has a reader on. Not on a view-filter frame:
	// its first scene pass renders into the filter's texture.
	void armSceneTap(Bool viewFilterActive)
	{
		s.tapFrame = false;
		if (viewFilterActive || TheGlobalData->m_headless)
			return;
		IDirect3DDevice8 *dev = DX8Wrapper::_Get_D3D_Device8();
		if (dev == nullptr)
			return;
		if (!s.tapAsked)
		{
			s.tapAsked = true;
			void *p = nullptr;
			if (SUCCEEDED(dev->QueryInterface(IID_GxSceneTap, &p)))
				s.tap = static_cast<IGxSceneTap *>(p);
			if (!s.hookSet)	// release() drops the interface with the device's other D3DPOOL_DEFAULT state
			{
				DX8Wrapper::Default_Pool_Release_Hook = &W3DWorldSeam::release;
				s.hookSet = true;
			}
		}
		s.tapFrame = s.tap != nullptr && s.tap->Wanted();
	}

	void cameraFields(CameraClass *cam, GxWorldPassBegin &b)
	{
		Matrix3D view;
		cam->Get_View_Matrix(&view);
		const Matrix4x4 v = Matrix4x4(view).Transpose();	// what DX8Wrapper::Set_Transform(D3DTS_VIEW, Matrix3D) sends
		Matrix4x4 p;
		cam->Get_D3D_Projection_Matrix(&p);
		const Matrix4x4 pt = p.Transpose();					// CameraClass::Apply -> Set_Transform(D3DTS_PROJECTION, Matrix4x4)
		memcpy(&b.view, &v, sizeof(b.view));
		memcpy(&b.proj, &pt, sizeof(b.proj));
		cam->Get_Clip_Planes(b.nearZ, b.farZ);
		b.fovYRad = cam->Get_Vertical_FOV();
	}
}

void W3DWorldSeam::release()
{
#ifdef GX_RENDER_NATIVE
	if (GxMotionUnits::Key && s.wp != nullptr)   // a native device answered IGxWorldPass: the bridge can own something
		GxMotionUnits::Release();
#endif
	releaseRef(s.texSurf);
	releaseRef(s.tex);
	releaseRef(s.msaa);
	releaseRef(s.depth);
	dropSaved();
	releaseOutTargets();
	if (s.wp != nullptr)
		s.wp->NextTargetsAreWorld(0);
	releaseRef(s.wp);
	s.wpAsked = false;
	releaseRef(s.tap);	// F-depth+: asked again from the next push (a reset device, or a new one)
	s.tapAsked = false;
	s.tapFrame = false;
	s.active = false;
	s.rw = s.rh = 0;
	s.resetHistory = true;
	// a bias still set (the hook ran between push and pop) is restored now: the Reset that follows may be skipped or fail before
	// it clears the state; the device can already be gone in Release_Device
	if (s.lodBias != 0.0f && DX8Wrapper::_Get_D3D_Device8() != nullptr)
		setLodBias(0.0f);
	s.lodBias = 0.0f;
}

Real W3DWorldSeam::activeScale()
{
	return (s.active && s.outH > 0 && s.rh != s.outH) ? (Real)s.rh / (Real)s.outH : 1.0f;
}

IGxWorldPass *W3DWorldSeam::worldPass()
{
	return s.active ? s.wp : nullptr;
}

void W3DWorldSeam::push(CameraClass *camera, Bool viewFilterActive)
{
	armSceneTap(viewFilterActive);	// F-depth+: seam on or off
	s.active = false;
	if (s.failed)
		return;
	const Int up = GXGraphics::upscaler();
	if (up == GXGraphics::UPSCALER_OFF || TheGlobalData->m_headless || camera == nullptr)
		return;
	if (viewFilterActive)
	{
		// Trade-off: a script view filter (B&W, motion blur, crossfade) draws display-pixel quads into the current target
		// (W3DShaderManager.cpp:384-406...), so its frames render full size as today; size-aware filters if one must run scaled.
		logState("state=bypass (script view filter)");
		return;
	}

	DX8Ring::Drain();	// d3d8 ring spike: direct device calls below (as Pillarbox_Begin)
	IDirect3DDevice8 *dev = DX8Wrapper::_Get_D3D_Device8();
	if (dev == nullptr || dev->TestCooperativeLevel() != D3D_OK)
		return;
	if (!s.hookSet)
	{
		DX8Wrapper::Default_Pool_Release_Hook = &W3DWorldSeam::release;
		s.hookSet = true;
	}
	if (!s.wpAsked)
	{
		s.wpAsked = true;
		void *p = nullptr;
		if (SUCCEEDED(dev->QueryInterface(IID_GxWorldPass, &p)))
			s.wp = static_cast<IGxWorldPass *>(p);
	}

	// The replaced target: the back buffer at game resolution, or the pillarbox surface. Its size, its colour format and its
	// depth's format and sample count are what the world drew into before the seam existed.
	int curW = 0, curH = 0, bits = 0;
	bool windowed = true;
	WW3D::Get_Render_Target_Resolution(curW, curH, bits, windowed);
	DX8Wrapper::Get_Render_Target_Pair(s.savedRT, s.savedDS);	// AddRef'd; released in pop(); a null RT = the back buffer
	D3DSURFACE_DESC dd = {}, cd = {};
	if (FAILED(dev->GetRenderTarget(&s.boundRT)) || s.boundRT == nullptr || FAILED(s.boundRT->GetDesc(&cd)) ||
		s.savedDS == nullptr || FAILED(s.savedDS->GetDesc(&dd)) || cd.Format == D3DFMT_UNKNOWN || curW <= 0 || curH <= 0)
	{
		dropSaved();
		logState("state=bypass (no colour or depth target)");
		return;
	}
	const Int pct = GXGraphics::renderScalePercent();
	const int rw = (curW * pct + 50) / 100 > 1 ? (curW * pct + 50) / 100 : 1;
	const int rh = (curH * pct + 50) / 100 > 1 ? (curH * pct + 50) / 100 : 1;
	const bool temporal = GXGraphics::upscalerIsTemporal(up);
	const D3DMULTISAMPLE_TYPE ms = temporal ? D3DMULTISAMPLE_NONE : dd.MultiSampleType;
	// the camera's viewport in both targets, checked before any target work: a collapsed one (a 2-pixel view at 33% truncates to 0
	// texels) has nothing to composite and would divide by zero - that frame renders as without the seam
	Vector2 vmin, vmax;
	camera->Get_Viewport(vmin, vmax);
	s.src = viewportRect(vmin, vmax, rw, rh);
	s.dst = viewportRect(vmin, vmax, curW, curH);
	if (s.src.right <= s.src.left || s.src.bottom <= s.src.top || s.dst.right - s.dst.left < s.src.right - s.src.left ||
		s.dst.bottom - s.dst.top < s.src.bottom - s.src.top)
	{
		dropSaved();
		logState("state=bypass (empty viewport)");
		return;
	}
	if (!ensureTargets(dev, rw, rh, ms, cd.Format, dd.Format, temporal ? (uint32_t)GXWP_TEMPORAL : 0u))
	{
		dropSaved();
		return;
	}
	if (!DX8Wrapper::Set_Render_Target_Pair(s.msaa != nullptr ? s.msaa : s.texSurf, s.depth))	// keeps IsRenderToTexture (false)
	{
		restoreTarget(dev);
		failOff("world target bind", E_FAIL);
		return;
	}
	const D3DVIEWPORT8 vp = { 0, 0, (DWORD)rw, (DWORD)rh, 0.0f, 1.0f };
	setViewport(vp);
	// The replaced target's own clear, on both paths: Begin_Render (W3DDisplay.cpp:3653 -> ww3d.cpp:851-864) clears the bound target
	// (the pillarbox surface too: Pillarbox_Begin runs first) with colour 0 and alpha m_minWaterOpacity, z 1, stencil 0 where the
	// depth has stencil (DX8Wrapper::Clear's own rule).
	Renderer::Clear(true, true, Vector3(0.0f, 0.0f, 0.0f), TheWaterTransparency->m_minWaterOpacity);

	// R-SL2: a stage-0 bias already set here is one an earlier pop() left behind (the arm GX_SEAM_LODBIAS=keep plants it)
	DWORD was = 0;
	if (!s.lodLeakLogged && dev->GetTextureStageState(0, D3DTSS_MIPMAPLODBIAS, &was) == D3D_OK && was != 0)
	{
		fprintf(stderr, "[GXSEAM] ERROR texture LOD bias 0x%08X on stage 0 at the world pass start: it leaked past a pop()\n", (unsigned)was);
		s.lodLeakLogged = true;
	}
	// the bias follows the upscaler that will actually run: pop() falls back to bilinear (no bias) when the device has none or its
	// output targets failed; a one-off dispatch failure there costs one biased bilinear frame
	const bool deviceUp = s.wp != nullptr && up > GXGraphics::UPSCALER_BILINEAR && !s.outFailed && !s.upFailed &&
		s.wp->Supports((uint32_t)(up - 1));
	const float bias = lodBiasMode() == 'o' || !deviceUp ? 0.0f : worldLodBias(up, s.src.right - s.src.left, s.dst.right - s.dst.left);
	if (bias != 0.0f)
		setLodBias(bias);

	s.outW = curW;
	s.outH = curH;
	s.pillarbox = s.savedRT != nullptr;
	s.upscaler = up;
	if (s.wp != nullptr)
	{
		GxWorldPassBegin b = {};
		b.colour = s.msaa != nullptr ? s.msaa : s.texSurf;
		b.depth = s.depth;
		b.renderW = (uint32_t)rw;
		b.renderH = (uint32_t)rh;
		b.outputW = (uint32_t)(s.dst.right - s.dst.left);
		b.outputH = (uint32_t)(s.dst.bottom - s.dst.top);
		b.flags = (s.resetHistory ? GXWP_RESET_HISTORY : 0u) | (temporal ? GXWP_TEMPORAL : 0u);
		b.viewX = (uint32_t)s.src.left;
		b.viewY = (uint32_t)s.src.top;
		b.viewW = (uint32_t)(s.src.right - s.src.left);
		b.viewH = (uint32_t)(s.src.bottom - s.src.top);
		camera->Get_Zbuffer_Range(b.minZ, b.maxZ);
		cameraFields(camera, b);
		s.wp->Begin(b);
#ifdef GX_RENDER_NATIVE
		if (GxMotionUnits::Key)
			GxMotionUnits::Begin(s.wp, b);   // the one ordered sequence (DLSS_INTEGRATION.md 01:08Z): after Begin ...
#endif
	}
	s.resetHistory = false;
	s.active = true;
	++s.engaged;
}

void W3DWorldSeam::pop()
{
	s.tapFrame = false;	// F-depth+: an untapped frame (no default scene pass ran) stays untapped
	if (!s.active)
		return;
	s.active = false;
	DX8Ring::Drain();
	IDirect3DDevice8 *dev = DX8Wrapper::_Get_D3D_Device8();
	if (s.wp != nullptr)
	{
#ifdef GX_RENDER_NATIVE
		if (GxMotionUnits::Key)
			GxMotionUnits::End();   // ... and before End, whose resolve reads the object velocity it registers
#endif
		s.wp->End();
	}
	// R-SL2: the world pass is over; the composite and the HUD sample at bias 0 (GX_SEAM_LODBIAS=keep = the planted leak)
	const float drewBias = s.lodBias;
	if (s.lodBias != 0.0f && lodBiasMode() != 'k')
		setLodBias(0.0f);

	const char *resolve = "none";
	HRESULT resolved = D3D_OK;
	if (s.msaa != nullptr)
	{
		// At 1:1 today's MSAA world is resolved in linear light by the present (GxNativePresent); under pillarbox by Pillarbox_End's
		// CopyRects (gamma). Match whichever path this frame replaced; classic has only CopyRects.
		if (s.wp != nullptr && !s.pillarbox && SUCCEEDED(s.wp->ResolveLinear(s.msaa, s.texSurf)))
			resolve = "linear";
		else
		{
			// a plain resolve, the one the pillarbox path ships (dx8wrapper.cpp:482-489)
			resolved = dev->CopyRects(s.msaa, nullptr, 0, s.texSurf, nullptr);
			resolve = "copyrects";
		}
	}

	// /P2-6: an unresolved world (stale or unwritten colour) or a target still bound as the world is
	// never sampled or drawn on; the seam goes OFF, this frame's world stays Begin_Render's clear
	const bool restored = restoreTarget(dev);
	if (FAILED(resolved) || !restored)
	{
		failOff(FAILED(resolved) ? "world MSAA resolve" : "output target restore", FAILED(resolved) ? resolved : E_FAIL);
		return;
	}

	bool device = false, tried = false, sharpened = false;
	const int ow = s.dst.right - s.dst.left, oh = s.dst.bottom - s.dst.top;
	if (s.wp != nullptr && s.upscaler > GXGraphics::UPSCALER_BILINEAR && s.wp->Supports((uint32_t)(s.upscaler - 1)) &&
		ensureOutTargets(dev, ow, oh, s.colourFmt))
	{
		GxUpscaleCall c = {};
		c.src = s.texSurf;
		c.srcW = (uint32_t)s.rw;
		c.srcH = (uint32_t)s.rh;
		c.srcRect = s.src;
		c.dst = s.outSurf;
		c.dstRect.left = 0;
		c.dstRect.top = 0;
		c.dstRect.right = ow;
		c.dstRect.bottom = oh;
		c.scratch = s.scrSurf;
		c.kind = (uint32_t)(s.upscaler - 1);
		c.sharpness = (float)GXGraphics::upscaleSharpness() / 100.0f;
		c.flags = GXGraphics::upscalerIsTemporal(s.upscaler) ? GXWP_TEMPORAL : 0u;
		device = SUCCEEDED(s.wp->Upscale(c));
		tried = true;
		// the ONE sharpener: a backend that does not sharpen itself (DLSS) gets RCAS on its output, written into the scratch texture
		if (device && rcasMode() != 'o' && (rcasMode() == 'a' || !s.wp->SharpensItself(c.kind)))
		{
			GxUpscaleCall r = c;
			r.src = s.outSurf;
			r.srcW = (uint32_t)ow;
			r.srcH = (uint32_t)oh;
			r.srcRect = c.dstRect;
			r.dst = s.scrSurf;
			r.scratch = nullptr;
			r.kind = GX_UPSCALE_RCAS;
			sharpened = SUCCEEDED(s.wp->Upscale(r));
		}
	}
	// a dispatch that failed keeps the next world unbiased until one succeeds again (pop() retries every frame), so a repeating
	// failure never biases the bilinear fallback for more than the frame that found it
	s.upFailed = tried && !device;
	if (device)
	{
		const RECT whole = { 0, 0, ow, oh };
		drawQuad(dev, sharpened ? s.scrTex : s.outTex, ow, oh, whole);	// the device's output, copied exactly into the camera's viewport
	}
	else
		drawQuad(dev, s.tex, s.rw, s.rh, s.src);
	if (s.wp != nullptr)
		s.wp->HudBegin();

	char state[256];
	snprintf(state, sizeof(state), "state=on upscaler=%s%s world=%dx%d out=%dx%d msaa=%d resolve=%s path=%s target=%s lodbias=%.3f",
		GXGraphics::upscalerName(s.upscaler),
		!device && s.upscaler > GXGraphics::UPSCALER_BILINEAR ? " (not in this session: bilinear)" : "",
		s.rw, s.rh, s.outW, s.outH, (int)s.ms, resolve,
		device ? (sharpened ? "device+rcas" : "device") : (s.rw == s.outW && s.rh == s.outH ? "point" : "bilinear"), s.pillarbox ? "pillarbox" : "1:1", drewBias);
	logState(state);
	if ((s.engaged % 3600) == 0)
		fprintf(stderr, "[GXSEAM] engaged=%u\n", s.engaged);
}

void W3DWorldSeam::opaqueDone()
{
	if (!s.tapFrame)
		return;
	s.tapFrame = false;
	DX8Ring::Drain();	// d3d8 ring spike: a direct device call
	s.tap->OpaqueDone();
}
