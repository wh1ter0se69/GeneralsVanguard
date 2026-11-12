/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
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

// IGxWorldPass on the native device (GxWorldPass.h). W3DWorldSeam calls these around the world pass; each
// recording body is an S body: SScope drains the worker and the body runs inline. The S comes BEFORE any read
// of image() or attachmentView() (a level's view is created lazily; a back buffer follows the rotation).

#include "GxNativeDevice.h"

#include <cstring>
#include <optional>
#include <string>

namespace GXRender { namespace Dev {

namespace R = GXRender::Res;
namespace V = GXRender::Vk;

uint32_t GxNativeDevice::SeamSlots::take(V::VkCtx& ctx, uint64_t now)
{
	const uint32_t slot = next;
	if (slot >= lastUse.size() || lastUse[slot] == now)
		return ~0u;
	// < now: submitted (a foreign submission inside an open frame is fatal, GxPacketWriter.h). A failed wait leaves it possibly
	// pending: never overwrite its set
	if (lastUse[slot] && !ctx.queue(V::QueueKind::Graphics).timeline.wait(lastUse[slot]))
		return ~0u;
	lastUse[slot] = now;   // before recording: a pass that fails half-way may still have bound the set
	next = (slot + 1) % uint32_t(lastUse.size());
	return slot;
}

HRESULT GxNativeDevice::WorldPass::NextTargetsAreWorld(uint32_t flags)
{
	// the next MSAA CreateRenderTarget is SAMPLED (ResolveLinear) only when the seam says it will make one; 0 disarms (the motion code
	// owns the depth half under GXWP_TEMPORAL)
	dev->m_worldNextColour = (flags & GXWP_MSAA_COLOUR) != 0;
	dev->worldNextTargets(flags);   // the next depth-stencil is sampleable under GXWP_TEMPORAL; 0 disarms
	return S_OK;
}

HRESULT GxNativeDevice::WorldPass::Begin(const GxWorldPassBegin& b)
{
	GxMotionBegin mb;   // GxMotion.h: a no-op while the world depth was never made sampleable (key off)
	mb.renderW = b.renderW;
	mb.renderH = b.renderH;
	mb.outputW = b.outputW;
	mb.outputH = b.outputH;
	mb.viewX = b.viewX;
	mb.viewY = b.viewY;
	mb.viewW = b.viewW;
	mb.viewH = b.viewH;
	mb.minZ = b.minZ;
	mb.maxZ = b.maxZ;
	mb.resetHistory = (b.flags & GXWP_RESET_HISTORY) != 0;
	mb.temporal = (b.flags & GXWP_TEMPORAL) != 0;
	std::memcpy(mb.view, &b.view, 64);
	std::memcpy(mb.proj, &b.proj, 64);
	mb.nearZ = b.nearZ;
	mb.farZ = b.farZ;
	mb.fovYRad = b.fovYRad;
	dev->worldBegin(mb, b.colour, b.depth);
	return S_OK;
}

HRESULT GxNativeDevice::WorldPass::End()
{
	dev->worldEnd();   // the velocity resolve (one outside-scope command) + the clean projection back
	return S_OK;
}

// The world target's MSAA colour resolved the way the native present resolves an MSAA back buffer: GxNativePresent's own pipeline
// (DXVK's swapchain blitter, gx_present_ms.frag: each sample decoded to linear light, averaged, encoded) on a second instance whose
// descriptor slots are its own (SeamSlots: a slot is reused only after the submission that last used it has completed). Any
// precondition missing = E_NOTIMPL, and the seam resolves with CopyRects.
HRESULT GxNativeDevice::WorldPass::ResolveLinear(IDirect3DSurface8* msaaSrc, IDirect3DSurface8* dstSurface)
{
	GxNativeDevice& d = *dev;
	if (d.m_worldResolveSlots == ~0u)   // declined for the session (front: written only below, inside the S): no drain
		return E_NOTIMPL;
	std::optional<SScope> sc;
	if (!d.m_debug.seamNoSResolve)   // (the plant: X-seam-resolve)
		sc.emplace(d, "seam ResolveLinear");
	GxNativeSurface* src = static_cast<GxNativeSurface*>(msaaSrc);
	GxNativeSurface* dst = static_cast<GxNativeSurface*>(dstSurface);
	R::GxRenderTarget* srt = src ? src->renderTarget() : nullptr;
	R::GxImage* si = src ? src->image() : nullptr;
	R::GxImage* di = dst ? dst->image() : nullptr;
	const VkImageView dv = dst ? dst->attachmentView() : VK_NULL_HANDLE;
	if (!srt || !srt->sampled() || !si || !di || !dv || si->samples == VK_SAMPLE_COUNT_1_BIT || di->samples != VK_SAMPLE_COUNT_1_BIT ||
		si->width != di->width || si->height != di->height || !d.m_haveSwapchain || d.m_presentWaits.empty())
		return E_NOTIMPL;
	const uint32_t slots = uint32_t(d.m_presentWaits.size());
	if (d.m_worldResolveSlots != slots) {   // first use, or the swapchain was rebuilt with another frames-in-flight count (~0u: tested above)
		d.m_worldResolve.shutdown();
		std::string why;
		if (!d.m_worldResolve.init(d.m_ctx, slots, &why)) {
			d.m_worldResolve.shutdown();
			d.m_worldResolveSlots = ~0u;
			d.logLine("[GXSEAM] native ResolveLinear unavailable: %s - the seam resolves with CopyRects", why.c_str());
			return E_NOTIMPL;
		}
		d.m_worldResolveSlots = slots;
		d.m_worldResolveUse.reset(slots);
	}
	d.ensureFrame();   // nextUseValue() is the open frame's submission from here
	const uint32_t slot = d.m_worldResolveUse.take(d.m_ctx, d.m_gx.nextUseValue());
	if (slot == ~0u)
		return E_NOTIMPL;
	bool ok = false;
	R::GxImage* touched[2] = { si, di };
	d.m_writer.outsideScope([&](VkCommandBuffer cmd) {
		ok = d.m_worldResolve.record(cmd, slot, *si, srt->attachmentView(), *di, dv, true);   // dst stays a colour attachment
	}, touched, 2);
	if (!ok)
		return E_FAIL;
	++d.m_stats.resolves;
	if (dst->kind() == GxNativeSurface::Kind::TextureLevel)
		dst->texture()->noteWritten(dst->level());
	return S_OK;
}

BOOL GxNativeDevice::WorldPass::Supports(uint32_t kind)
{
	if (kind == GX_UPSCALE_DLSS)
		return dev->dlssSupported() ? TRUE : FALSE;   // (GxNativeDlss.cpp)
	// front-side: a presenting device whose FSR 1 passes did not fail to initialise (RCAS alone is FSR 1's second pass)
	return (kind == GX_UPSCALE_FSR1 || kind == GX_UPSCALE_RCAS) && dev->m_haveSwapchain && dev->m_fsr1Slots != ~0u;
}

BOOL GxNativeDevice::WorldPass::SharpensItself(uint32_t kind)
{
	// FSR 1 ends in RCAS and FSR 3.1 in its own RCAS; DLSS (deprecated sharpening), MetalFX and XeSS do not
	return kind == GX_UPSCALE_FSR1 || kind == GX_UPSCALE_FSR3;
}

bool GxNativeDevice::WorldPass::fsr1Ready()
{
	GxNativeDevice& d = *dev;
	const uint32_t slots = uint32_t(d.m_presentWaits.size());
	if (d.m_fsr1Slots == slots)
		return true;
	if (d.m_fsr1Slots == ~0u || !slots)
		return false;
	d.m_fsr1.shutdown();
	std::string why;
	if (!d.m_fsr1.init(d.m_ctx, slots, &why)) {
		d.m_fsr1.shutdown();
		d.m_fsr1Slots = ~0u;
		d.logLine("[GXSEAM] native FSR 1 unavailable: %s - the seam upscales bilinearly and does not sharpen", why.c_str());
		return false;
	}
	d.m_fsr1Slots = slots;
	d.m_fsr1Use.reset(slots);
	return true;
}

// The seam's ONE sharpener after a backend that does not sharpen itself: FSR 1's RCAS alone, c.src
// -> c.dst 1:1, both the seam's output-size textures; FSR 1's SeamSlots (the RCAS half of a slot). Anything else = E_NOTIMPL.
HRESULT GxNativeDevice::WorldPass::sharpen(const GxUpscaleCall& c)
{
	GxNativeDevice& d = *dev;
	GxNativeSurface* src = static_cast<GxNativeSurface*>(c.src);
	GxNativeSurface* dst = static_cast<GxNativeSurface*>(c.dst);
	R::GxImage* si = src ? src->image() : nullptr;
	R::GxImage* di = dst ? dst->image() : nullptr;
	const VkImageView sv = src ? src->attachmentView() : VK_NULL_HANDLE;
	const VkImageView dv = dst ? dst->attachmentView() : VK_NULL_HANDLE;
	if (!si || !di || !sv || !dv || si == di || si->samples != VK_SAMPLE_COUNT_1_BIT || di->samples != VK_SAMPLE_COUNT_1_BIT ||
		si->width != di->width || si->height != di->height || !fsr1Ready())
		return E_NOTIMPL;
	d.ensureFrame();
	const uint32_t slot = d.m_fsr1Use.take(d.m_ctx, d.m_gx.nextUseValue());
	if (slot == ~0u)
		return E_NOTIMPL;
	bool ok = false;
	R::GxImage* touched[2] = { si, di };
	d.m_writer.outsideScope([&](VkCommandBuffer cmd) {
		ok = d.m_fsr1.recordRcas(cmd, slot, *si, sv, *di, dv, c.sharpness);
	}, touched, 2);
	if (!ok)
		return E_FAIL;
	if (dst->kind() == GxNativeSurface::Kind::TextureLevel)
		dst->texture()->noteWritten(dst->level());
	return S_OK;
}

// FSR 1 (GxFsr1.h): EASU world -> scratch, RCAS scratch -> dst, both the seam's output-size textures. Its own SeamSlots, like
// ResolveLinear. Anything else = E_NOTIMPL (bilinear).
HRESULT GxNativeDevice::WorldPass::Upscale(const GxUpscaleCall& c)
{
	GxNativeDevice& d = *dev;
	if (!Supports(c.kind))   // front state (m_haveSwapchain, m_fsr1Slots, DLSS's support answer): no drain for a kind that cannot run
		return E_NOTIMPL;
	// every backend below this line runs inside the S (DLSS too: it stays forced off, so its S is the CS-off re-seed only); sharpen()
	// is nested at depth 2 - no second drain
	std::optional<SScope> sc;
	if (!d.m_debug.seamNoSUpscale)   // (the plant: X-seam-fsr1 / X-seam-rcas)
		sc.emplace(d, "seam Upscale");
	if (c.kind == GX_UPSCALE_DLSS)
		return d.dlssUpscale(c);   // (GxNativeDlss.cpp)
	if (c.kind == GX_UPSCALE_RCAS)
		return sharpen(c);
	GxNativeSurface* src = static_cast<GxNativeSurface*>(c.src);
	GxNativeSurface* scr = static_cast<GxNativeSurface*>(c.scratch);
	GxNativeSurface* dst = static_cast<GxNativeSurface*>(c.dst);
	R::GxImage* si = src ? src->image() : nullptr;
	R::GxImage* ci = scr ? scr->image() : nullptr;
	R::GxImage* di = dst ? dst->image() : nullptr;
	const VkImageView sv = src ? src->attachmentView() : VK_NULL_HANDLE;
	const VkImageView cv = scr ? scr->attachmentView() : VK_NULL_HANDLE;
	const VkImageView dv = dst ? dst->attachmentView() : VK_NULL_HANDLE;
	const LONG w = c.dstRect.right - c.dstRect.left, h = c.dstRect.bottom - c.dstRect.top;
	if (!si || !ci || !di || !sv || !cv || !dv || si == ci || si == di || ci == di || si->samples != VK_SAMPLE_COUNT_1_BIT ||
		ci->samples != VK_SAMPLE_COUNT_1_BIT || di->samples != VK_SAMPLE_COUNT_1_BIT || c.dstRect.left != 0 || c.dstRect.top != 0 ||
		w <= 0 || h <= 0 || uint32_t(w) != di->width || uint32_t(h) != di->height || ci->width != di->width || ci->height != di->height ||
		c.srcRect.left < 0 || c.srcRect.top < 0 || c.srcRect.right <= c.srcRect.left || c.srcRect.bottom <= c.srcRect.top ||
		uint32_t(c.srcRect.right) > si->width || uint32_t(c.srcRect.bottom) > si->height || d.m_presentWaits.empty())
		return E_NOTIMPL;
	if (!fsr1Ready())
		return E_NOTIMPL;
	d.ensureFrame();
	const uint32_t slot = d.m_fsr1Use.take(d.m_ctx, d.m_gx.nextUseValue());
	if (slot == ~0u)
		return E_NOTIMPL;
	const VkRect2D sr = { { c.srcRect.left, c.srcRect.top }, { uint32_t(c.srcRect.right - c.srcRect.left), uint32_t(c.srcRect.bottom - c.srcRect.top) } };
	bool ok = false;
	R::GxImage* touched[3] = { si, ci, di };
	d.m_writer.outsideScope([&](VkCommandBuffer cmd) {
		ok = d.m_fsr1.record(cmd, slot, *si, sv, sr, *ci, cv, *di, dv, c.sharpness);
	}, touched, 3);
	if (!ok)
		return E_FAIL;
	for (GxNativeSurface* s : { scr, dst })
		if (s->kind() == GxNativeSurface::Kind::TextureLevel)
			s->texture()->noteWritten(s->level());
	return S_OK;
}

HRESULT GxNativeDevice::WorldPass::HudBegin()
{
	// No-op unless TEST-ONLY GX_FG_HUDLESS=1 with frame generation on. Its state lives in m_dlss, which exists
	// only with a loaded Streamline - and a loaded Streamline keeps the worker off (the guard), so this S never drains today; it keeps
	// fgOn (written by slFrameEnd in the Present body) read inside an S if that ever changes. No Streamline (every Apple build): no S.
	if (!dev->m_dlss)
		return S_OK;
	SScope sc(*dev, "seam HudBegin");
	dev->fgHudBegin();
	return S_OK;
}

HRESULT GxNativeDevice::WorldPass::SetObjectVelocity(IDirect3DSurface8* o)
{
	return dev->worldObjectVelocity(o);   // End's resolve takes o.xy where o.a > 0.5; null cancels
}

HRESULT GxNativeDevice::WorldPass::GetJitter(float ndc[2])
{
	dev->worldJitter(ndc);   // this world pass's projection jitter in NDC, front state ((0,0) when none)
	return S_OK;
}

} } // namespace GXRender::Dev
