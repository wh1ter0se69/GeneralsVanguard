// GeneralsX: IGxSceneTap on the native device (GxSceneTap.h; F-depth+, GxSceneCopy.h).
//
// Front halves here; the copies are ONE async command per frame (A23-B: owned arguments, stream-ordered with the draws around it -
// it runs on gx-native-cs when the worker is on, which is wh1ter0se's default with GXUpscaler off).

#include "GxNativeDevice.h"

#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace GXRender { namespace Dev {

namespace R = GXRender::Res;

HRESULT GxNativeDevice::sceneSetReader(const char* name, uint32_t needs)
{
	if (!m_sceneInit || !name)
		return E_INVALIDARG;
	if (!m_scene.readers().set(name, needs))
		return S_OK;
	logLine("[GXFDEPTH] readers: %s", m_scene.readers().describe().c_str());
	if (m_scene.readers().needs() == 0)
		cs([this] { m_scene.disarm(m_writer); });   // the world's scopes stop resolving from the next one on
	return S_OK;
}

HRESULT GxNativeDevice::sceneOpaqueDone()
{
	if (!m_sceneInit)
		return S_OK;
	GxScenePlants& pl = m_scene.plants();
	pl.tapWithoutReader = m_debug.fdepthTapWithoutReader;
	pl.secondTapRuns = m_debug.fdepthSecondTap;
	pl.fallbackRecordsNothing = m_debug.fdepthFallbackNothing;
	uint32_t needs = m_scene.readers().needs();
	if (!needs && pl.tapWithoutReader)
		needs = GX_SCENE_DEPTH | GX_SCENE_COLOUR;   // the plant
	if (!needs || (m_sceneTapped && !pl.secondTapRuns))
		return S_OK;   // no reader: nothing created, armed or recorded; a second call in a frame: the first one's copies stand
	m_sceneTapped = true;
	// the bound pair, front side (the immutable description: creation runs on the front, A23-B §3.1)
	GxNativeSurface* const rt = m_rt;
	GxNativeSurface* const ds = m_ds && m_ds->kind() == GxNativeSurface::Kind::Depth ? m_ds : nullptr;
	std::vector<R::GxRenderTarget*> old;
	std::string line;
	const GxSceneCopy::Tap t = m_scene.prepare(m_gx, needs, ds ? ds->d3dFormat() : 0u, ds ? ds->width() : 0u, ds ? ds->height() : 0u,
		rt ? rt->d3dFormat() : 0u, rt ? rt->width() : 0u, rt ? rt->height() : 0u, &old, &line);
	if (!line.empty())
		logLine("%s", line.c_str());
	for (R::GxRenderTarget* o : old)   // behind every command that named it
		cs([this, o] { GxSceneCopy::retire(m_writer, o); });
	cs([this, t] {
		ensureFrame();
		// the WORKER's binding (A23-B §3.5) = the front's at the call, in stream order; a back buffer's storage is the rotation's
		GxNativeSurface* const c = m_wRt;
		GxNativeSurface* const d = m_wDs && m_wDs->kind() == GxNativeSurface::Kind::Depth ? m_wDs : nullptr;
		R::GxImage* const ci = c ? c->image() : nullptr;
		R::GxImage* const di = d ? d->image() : nullptr;
		const bool first = m_scene.stats().taps == 0;
		m_scene.tap(m_writer, t, ci, di, di ? d->attachmentView() : VK_NULL_HANDLE);
		if (first) {   // the usage words: the world's are F-depth+'s inputs only (never changed by it), the copies' its own
			const GxSceneFrame& f = m_scene.frame();
			logLine("[GXFDEPTH] on: world colour %s x%u usage=0x%x, depth %s x%u usage=0x%x; took depth=%d(path %u) colour=%d(path %u)",
				ci && ci->format ? ci->format->name : "-", ci ? unsigned(ci->samples) : 0u, ci ? unsigned(ci->usage) : 0u,
				di && di->format ? di->format->name : "-", di ? unsigned(di->samples) : 0u, di ? unsigned(di->usage) : 0u,
				f.depth ? 1 : 0, unsigned(f.depthPath), f.colour ? 1 : 0, unsigned(f.colourPath));
		}
	});
	return S_OK;
}

void GxNativeDevice::logSceneStats()
{
	if (!m_sceneInit || m_scene.stats().taps == 0)
		return;
	const GxSceneStats& s = m_scene.stats();
	logLine("[GXFDEPTH] taps=%llu in_scope=%llu pass=%llu copy=%llu refused=%llu probes=%llu scope_resolves=%llu",
		(unsigned long long)s.taps, (unsigned long long)s.inScope, (unsigned long long)s.passes, (unsigned long long)s.copies,
		(unsigned long long)s.refused, (unsigned long long)s.probes, (unsigned long long)m_writer.stats().scopeResolves);
}

bool GxNativeDevice::testReadSceneProbe(float out[4])
{
	// E · seam-worker-2 (seam2 design r1 P2): an S body, not a bare csSync - endFrameForHost below is an ownership boundary, and a
	// drain alone (m_inline 0) counted it as a front recording whenever the worker was on
	std::optional<SScope> sc;
	if (m_debug.accessorBareSync)
		csSync("test read-back");   // (the plant: X-own-present-read)
	else
		sc.emplace(*this, "test read-back");
	R::GxImage* img = m_scene.probeImage();
	if (!img)
		return false;
	endFrameForHost();
	std::vector<uint8_t> px;
	if (!m_gx.readImage(*img, 0, nullptr, &px) || px.size() < 16)
		return false;
	std::memcpy(out, px.data(), 16);
	return true;
}

}} // namespace GXRender::Dev
