// GeneralsX @feature E · rw3-L 11/10/2026 L1: the native d3d8 resource objects (GxNativeDevice.h; DESIGN_l1-device.md §3, §6).
#include "GxNativeDevice.h"

#include "GxD3D.h"
#include "GxNativeUuid.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace GXRender { namespace Dev {

namespace R = GXRender::Res;

// ------------------------------------------------------------------------------------------------------------ references

ULONG GxNativeRefs::addPublic()
{
	const ULONG r = m_public++;
	if (r == 0) {   // d3d8_device_child.h AddRef: the first public reference holds one private and the device
		addPrivate();
		if (!m_owner.dbg().childrenDontRetain)
			m_owner.AddRef();
	}
	return r + 1;
}

ULONG GxNativeRefs::releasePublic()
{
	ULONG old = m_public.load(std::memory_order_acquire), n = 0;
	do {
		if (old == 0)
			return 0;   // clamped (DXVK: "prevent underruns")
		n = old - 1;
	} while (!m_public.compare_exchange_weak(old, n, std::memory_order_release, std::memory_order_acquire));
	if (n == 0) {
		GxNativeDevice* dev = &m_owner;   // releasePrivate may destroy this
		const bool retained = !dev->dbg().childrenDontRetain;   // ANY thread: never the draining debug() (A23-B §3.1)
		releasePrivate();
		if (retained)
			dev->Release();
	}
	return n;
}

void GxNativeRefs::releasePrivate()
{
	if (--m_private != 0 || containerOwned())
		return;
	if (m_owner.dbg().destroyImmediately)
		destroy();
	else
		m_owner.deferDestroy(this);
}

namespace {

bool isIID(REFIID a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

HRESULT noPrivateData() { return D3DERR_NOTFOUND; }

} // namespace

// ------------------------------------------------------------------------------------------------------------ surface

GxNativeSurface::GxNativeSurface(GxNativeDevice& dev, Kind kind, GxNativeTexture* tex, uint32_t level)
	: GxNativeRefs(dev), m_dev(dev), m_kind(kind), m_tex(tex), m_level(level)
{
	// a level surface's references are its container's (d3d8_subresource.h:27-39); the texture owns the object
	initFront();
}

GxNativeSurface::GxNativeSurface(GxNativeDevice& dev, Kind kind, std::unique_ptr<R::GxRenderTarget> rt, uint32_t d3dFormat,
	uint32_t usage)
	: GxNativeRefs(dev), m_dev(dev), m_kind(kind), m_rt(std::move(rt)), m_format(d3dFormat)
{
	++m_dev.frontStats().liveSurfaces;
	++m_dev.frontStats().losable;   // a DEFAULT-pool surface; the auto depth too until it is destroyed (DXVK d3d9_device.cpp:8650)
	initFront();
}

GxNativeSurface::GxNativeSurface(GxNativeDevice& dev, uint32_t backBufferIndex)
	: GxNativeRefs(dev), m_dev(dev), m_kind(Kind::BackBuffer), m_bb(backBufferIndex)
{
	++m_dev.frontStats().liveSurfaces;   // a standalone device child the device holds privately (d3d8_device.h:400-404, :458-462)
	++m_dev.frontStats().losable;   // DXVK counts every back buffer until it is destroyed (d3d9_swapchain.cpp:1035, Codex code-r3 #2)
	initFront();   // made right after the swapchain objects (direct): every storage has the same description
}

void GxNativeSurface::testUncountLosable()
{
	if (!m_notLosable) {
		m_notLosable = true;
		--m_dev.frontStats().losable;
	}
}

GxNativeSurface::GxNativeSurface(GxNativeDevice& dev, std::unique_ptr<R::GxTexture> image)
	: GxNativeRefs(dev), m_dev(dev), m_kind(Kind::Image), m_image(std::move(image))
{
	++m_dev.frontStats().liveSurfaces;
	initFront();
}

// What width() / height() / d3dFormat() / pool() / usage() / multisampled() / GetDesc returned from the live objects before
// A23-B, read once here: none of it changes over the surface's life (a back buffer's storages all share one description).
void GxNativeSurface::initFront()
{
	FrontDesc& f = m_front;
	switch (m_kind) {
	case Kind::TextureLevel: {
		const R::GxTexture& t = m_tex->gx();
		f.format = t.desc().format;
		f.width = t.levelWidth(m_level);
		f.height = t.levelHeight(m_level);
		f.pool = t.desc().pool;
		f.usage = t.desc().usage;
		f.hasImage = m_tex->gx().image().image != VK_NULL_HANDLE;
		return;
	}
	case Kind::Image:
		f.format = m_image->desc().format;
		f.width = m_image->desc().width;
		f.height = m_image->desc().height;
		f.pool = R::d3d::POOL_SYSTEMMEM;
		return;
	default: {
		R::GxRenderTarget* rt = renderTarget();
		f.format = m_kind == Kind::BackBuffer ? (rt && rt->image().format ? rt->image().format->d3d : 0) : m_format;
		f.width = rt ? rt->image().width : 0;
		f.height = rt ? rt->image().height : 0;
		f.pool = R::d3d::POOL_DEFAULT;
		f.usage = m_kind == Kind::Depth ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET;
		f.requestedSamples = rt ? rt->requestedSamples() : 0;
		f.multisampled = rt && rt->samples() != VK_SAMPLE_COUNT_1_BIT;
		f.samples = rt ? uint32_t(rt->samples()) : 1u;
		f.hasImage = rt != nullptr;
		return;
	}
	}
}

bool GxNativeSurface::frontHasImage() const
{
	if (m_kind == Kind::BackBuffer)
		return m_rt || m_dev.backBufferCount() > 0;
	return m_front.hasImage;
}

GxNativeSurface::~GxNativeSurface()
{
	if (m_rt) {
		m_dev.forgetImage(&m_rt->image());
		m_rt.reset();
	}
	if (m_image) {
		m_dev.forgetImage(&m_image->image());
		m_image.reset();
	}
}

void GxNativeSurface::destroyFront()
{
	if (m_kind == Kind::TextureLevel)
		return;   // owned by its texture
	--m_dev.frontStats().liveSurfaces;
	if ((m_kind == Kind::RenderTarget || m_kind == Kind::Depth || m_kind == Kind::BackBuffer) && !m_notLosable)
		--m_dev.frontStats().losable;
}

void GxNativeSurface::destroyStorage()
{
	if (m_kind != Kind::TextureLevel)
		delete this;   // ~GxNativeSurface: forgetImage + the target's / image's storage, retired at its last use
}

HRESULT GxNativeSurface::QueryInterface(REFIID riid, void** pp)
{
	if (!pp)
		return E_POINTER;
	if (isIID(riid, __uuidof(IUnknown)) || isIID(riid, __uuidof(IDirect3DSurface8))) {
		AddRef();
		*pp = static_cast<IDirect3DSurface8*>(this);
		return S_OK;
	}
	*pp = nullptr;
	return E_NOINTERFACE;
}

ULONG GxNativeSurface::AddRef()
{
	if (m_kind == Kind::TextureLevel && !m_dev.dbg().levelSurfaceOwnRef)
		return m_tex->AddRef();
	return addPublic();   // the plant: a level surface counting for itself (its container never hears of it)
}

ULONG GxNativeSurface::Release()
{
	if (m_kind == Kind::TextureLevel && !m_dev.dbg().levelSurfaceOwnRef)
		return m_tex->Release();
	return releasePublic();
}

void GxNativeSurface::addPrivateRef()
{
	if (m_kind == Kind::TextureLevel)
		m_tex->addPrivateRef();
	else
		addPrivate();
}

void GxNativeSurface::releasePrivateRef()
{
	if (m_kind == Kind::TextureLevel)
		m_tex->releasePrivateRef();
	else
		releasePrivate();
}

HRESULT GxNativeSurface::GetDevice(IDirect3DDevice8** pp)
{
	if (!pp)
		return D3DERR_INVALIDCALL;
	m_dev.AddRef();
	*pp = &m_dev;
	return D3D_OK;
}

HRESULT GxNativeSurface::SetPrivateData(REFGUID, const void*, DWORD, DWORD) { return m_dev.neverCalled("Surface::SetPrivateData"); }
HRESULT GxNativeSurface::GetPrivateData(REFGUID, void*, DWORD*) { return noPrivateData(); }
HRESULT GxNativeSurface::FreePrivateData(REFGUID) { return noPrivateData(); }

HRESULT GxNativeSurface::GetContainer(REFIID riid, void** pp)
{
	if (m_kind == Kind::TextureLevel)
		return m_tex->QueryInterface(riid, pp);
	return m_dev.QueryInterface(riid, pp);
}

R::GxRenderTarget* GxNativeSurface::renderTarget() const
{
	if (m_kind == Kind::BackBuffer && !m_rt)   // a detached back buffer (held across a Reset) owns its storage
		return m_dev.backBufferStorage(m_bb);
	return m_rt.get();
}

R::GxImage* GxNativeSurface::image() const
{
	switch (m_kind) {
	case Kind::TextureLevel: return m_tex->gx().image().image ? &m_tex->gx().image() : nullptr;
	case Kind::Image: return nullptr;
	default: {
		R::GxRenderTarget* rt = renderTarget();
		return rt ? &rt->image() : nullptr;
	}
	}
}

VkImageView GxNativeSurface::attachmentView() const
{
	if (m_kind == Kind::TextureLevel)
		return m_level == 0 ? m_tex->gx().attachmentView() : VK_NULL_HANDLE;
	R::GxRenderTarget* rt = renderTarget();
	return rt ? rt->attachmentView() : VK_NULL_HANDLE;
}

HRESULT GxNativeSurface::GetDesc(D3DSURFACE_DESC* d)
{
	if (!d)
		return D3DERR_INVALIDCALL;
	std::memset(d, 0, sizeof(*d));
	d->Format = D3DFORMAT(d3dFormat());
	d->Type = D3DRTYPE_SURFACE;
	d->Pool = D3DPOOL(pool());
	d->Width = width();
	d->Height = height();
	d->Usage = usage();
	if (m_kind != Kind::TextureLevel && m_kind != Kind::Image && m_front.hasImage)
		d->MultiSampleType = D3DMULTISAMPLE_TYPE(m_front.requestedSamples > 1 ? m_front.requestedSamples : 0);
	// X' (rw3-X-3): the immutable format table, never the draining resources() - every game GetDesc / GetLevelDesc drained the
	// worker (5.35 ms per frame at wh1ter0se's camera on Windows, gate 2). Everything read here is fixed at creation on the front: m_kind,
	// m_front (initFront, every constructor) and the format table (GxResources::init, before the worker starts).
	const R::FormatTable& ft = m_dev.dbg().csGetDescDrains ? m_dev.resources().formats() : m_dev.formats();   // (the plant)
	d->Size = UINT(ft.levelBytes(d3dFormat(), d->Width, d->Height));
	if (m_dev.dbg().csGetDescRace && m_kind == Kind::TextureLevel) {   // the plant (TSan-judged)
		static volatile uint64_t sink;
		sink = m_tex->gx().image().lastUse;
	}
	return D3D_OK;
}

HRESULT GxNativeSurface::LockRect(D3DLOCKED_RECT* lr, const RECT* rect, DWORD flags)
{
	if (m_kind == Kind::TextureLevel)
		return m_tex->LockRect(m_level, lr, rect, flags);
	if (m_kind != Kind::Image || !lr)
		return D3DERR_INVALIDCALL;   // render targets and depth are not lockable (created Lockable = FALSE)
	VkRect2D r = { { 0, 0 }, { m_image->desc().width, m_image->desc().height } };
	if (rect)
		r = { { rect->left, rect->top }, { uint32_t(rect->right - rect->left), uint32_t(rect->bottom - rect->top) } };
	R::LockedRect out;
	if (!m_image->lockRect(0, &r, flags, &out))
		return D3DERR_INVALIDCALL;
	lr->pBits = out.bits;
	lr->Pitch = INT(out.pitch);
	return D3D_OK;
}

HRESULT GxNativeSurface::UnlockRect()
{
	if (m_kind == Kind::TextureLevel)
		return m_tex->UnlockRect(m_level);
	if (m_kind != Kind::Image)
		return D3DERR_INVALIDCALL;
	m_image->unlockRect(0);
	return D3D_OK;
}

// ------------------------------------------------------------------------------------------------------------ texture

GxNativeTexture::GxNativeTexture(GxNativeDevice& dev, std::unique_ptr<R::GxTexture> tex)
	: GxNativeRefs(dev), m_dev(dev), m_tex(std::move(tex))
{
	m_levels.resize(m_tex->levels());
	m_writes.assign(m_tex->levels(), 0);
	m_dirty = RECT { 0, 0, LONG(m_tex->desc().width), LONG(m_tex->desc().height) };   // d3d9_common_texture.cpp:31
	++m_dev.frontStats().liveTextures;
	if (m_tex->desc().pool == R::d3d::POOL_DEFAULT)
		++m_dev.frontStats().losable;
}

GxNativeTexture::~GxNativeTexture()
{
	m_dev.forgetImage(&m_tex->image());
	m_levels.clear();
	m_tex.reset();
}

void GxNativeTexture::destroyFront()
{
	m_dev.unnoteManaged(this);   // the drop-candidate list is front state
	--m_dev.frontStats().liveTextures;
	if (m_tex->desc().pool == R::d3d::POOL_DEFAULT)
		--m_dev.frontStats().losable;
}

void GxNativeTexture::destroyStorage()
{
	delete this;
}

// d3d9_device.cpp:5083-5100 + d3d9_common_texture.h:405-430: a rect of `level` grows the level-0 box (scaled up and clamped);
// null = the whole texture.
void GxNativeTexture::addDirty(const RECT* r, uint32_t level)
{
	const LONG W = LONG(m_tex->desc().width), H = LONG(m_tex->desc().height);
	RECT b = { 0, 0, W, H };
	if (r) {
		b = *r;
		if (level != 0) {
			b.left <<= level;
			b.top <<= level;
			b.right = std::min<LONG>(b.right << level, W);
			b.bottom = std::min<LONG>(b.bottom << level, H);
		}
		if (b.right <= b.left || b.bottom <= b.top)
			return;
		b.right = std::min(b.right, W);
		b.bottom = std::min(b.bottom, H);
	}
	if (m_dirty.left == m_dirty.right)
		m_dirty = b;
	else {
		m_dirty.left = std::min(m_dirty.left, b.left);
		m_dirty.top = std::min(m_dirty.top, b.top);
		m_dirty.right = std::max(m_dirty.right, b.right);
		m_dirty.bottom = std::max(m_dirty.bottom, b.bottom);
	}
}

HRESULT GxNativeTexture::QueryInterface(REFIID riid, void** pp)
{
	if (!pp)
		return E_POINTER;
	if (isIID(riid, __uuidof(IUnknown)) || isIID(riid, __uuidof(IDirect3DResource8)) ||
		isIID(riid, __uuidof(IDirect3DBaseTexture8)) || isIID(riid, __uuidof(IDirect3DTexture8))) {
		AddRef();
		*pp = static_cast<IDirect3DTexture8*>(this);
		return S_OK;
	}
	*pp = nullptr;
	return E_NOINTERFACE;
}

ULONG GxNativeTexture::AddRef() { return addPublic(); }
ULONG GxNativeTexture::Release() { return releasePublic(); }

HRESULT GxNativeTexture::GetDevice(IDirect3DDevice8** pp)
{
	if (!pp)
		return D3DERR_INVALIDCALL;
	m_dev.AddRef();
	*pp = &m_dev;
	return D3D_OK;
}

HRESULT GxNativeTexture::SetPrivateData(REFGUID, const void*, DWORD, DWORD) { return m_dev.neverCalled("Texture::SetPrivateData"); }
HRESULT GxNativeTexture::GetPrivateData(REFGUID, void*, DWORD*) { return noPrivateData(); }
HRESULT GxNativeTexture::FreePrivateData(REFGUID) { return noPrivateData(); }

DWORD GxNativeTexture::SetPriority(DWORD p)
{
	const DWORD o = m_priority;
	m_priority = p;   // stored, no effect: DXVK evicts nothing on 64-bit (CENSUS_resources §0.2)
	return o;
}

DWORD GxNativeTexture::SetLOD(DWORD lod)
{
	// d3d9_texture.h:60-71: stores min(new, levels - 1), returns the previous one (GxTexture::setLod returns the previous)
	const DWORD o = m_lod;
	m_dev.setTextureLod(this, lod);   // A23-B §3.5: A - the front answers, the worker's view() reads the clamped value
	m_lod = m_dev.dbg().lodStoresPrevious ? o : std::min<DWORD>(lod, m_tex->levels() ? m_tex->levels() - 1 : 0);
	return o;
}

HRESULT GxNativeTexture::GetLevelDesc(UINT level, D3DSURFACE_DESC* desc)
{
	if (level >= m_tex->levels() || !desc)
		return D3DERR_INVALIDCALL;
	IDirect3DSurface8* s = nullptr;
	if (FAILED(GetSurfaceLevel(level, &s)))
		return D3DERR_INVALIDCALL;
	const HRESULT hr = s->GetDesc(desc);
	s->Release();
	return hr;
}

HRESULT GxNativeTexture::GetSurfaceLevel(UINT level, IDirect3DSurface8** pp)
{
	if (!pp || level >= m_tex->levels())
		return D3DERR_INVALIDCALL;
	if (!m_levels[level])
		m_levels[level].reset(new GxNativeSurface(m_dev, GxNativeSurface::Kind::TextureLevel, this, level));
	*pp = m_levels[level].get();
	(*pp)->AddRef();
	return D3D_OK;
}

void GxNativeTexture::pin()
{
	if (!m_pinned) {
		m_pinned = true;
		++m_dev.frontStats().pinned;
	}
}

void GxNativeTexture::noteWritten(uint32_t level)
{
	if (level < m_writes.size())
		++m_writes[level];
	if (managed() && (!m_pinned || m_dev.dbg().dropIgnoresPin))
		m_dev.noteManaged(this);
}

bool GxNativeTexture::dropCandidate() const
{
	if (!managed() || (m_pinned && !m_dev.dbg().dropIgnoresPin) || m_tex->decoded())
		return false;
	for (uint32_t l = 0; l < m_tex->levels(); ++l)
		if (m_writes[l] == 0 || !m_tex->hasHostCopy(l))
			return false;
	return true;
}

HRESULT GxNativeTexture::LockRect(UINT level, D3DLOCKED_RECT* lr, const RECT* rect, DWORD flags)
{
	if (level >= m_tex->levels() || !lr)
		return D3DERR_INVALIDCALL;
	// DESIGN §6: a level written before is being locked again - an incremental writer or a reader (D3DXFilterTexture): keep
	// the host copy from now on. A dropped level is restored from the GPU, which needs the writer frame closed first.
	if (managed() && m_writes[level] > 0)
		pin();
	std::unique_ptr<GxNativeDevice::SScope> sync;
	if (!m_tex->hasHostCopy(level) && m_tex->desc().pool != R::d3d::POOL_DEFAULT) {
		if (!m_dev.dbg().csRestoreSOnlyReadonly || (flags & D3DLOCK_READONLY))   // (the plant)
			sync.reset(new GxNativeDevice::SScope(m_dev, "LockRect restore"));   // A23-B §3.6: the restore reads the GPU - S
		m_dev.endFrameForHost();
		++m_dev.frontStats().restores;
	}
	VkRect2D r = { { 0, 0 }, { m_tex->levelWidth(level), m_tex->levelHeight(level) } };
	if (rect)
		r = { { rect->left, rect->top }, { uint32_t(rect->right - rect->left), uint32_t(rect->bottom - rect->top) } };
	R::LockedRect out;
	if (!m_tex->lockRect(level, &r, flags, &out))
		return D3DERR_INVALIDCALL;
	if (!(flags & D3DLOCK_READONLY) && (m_tex->desc().pool == R::d3d::POOL_DEFAULT || !(flags & D3DLOCK_NO_DIRTY_UPDATE)))
		addDirty(rect, level);
	lr->pBits = out.bits;
	lr->Pitch = INT(out.pitch);
	return D3D_OK;
}

HRESULT GxNativeTexture::UnlockRect(UINT level)
{
	if (level >= m_tex->levels())
		return D3DERR_INVALIDCALL;
	m_dev.unlockTextureLevel(this, level);   // unlock + noteWritten + the idle flush, split per A23-B §3.6
	return D3D_OK;
}

// ------------------------------------------------------------------------------------------------------------ buffers

template <class I, class Desc>
GxNativeBuffer<I, Desc>::GxNativeBuffer(GxNativeDevice& dev, std::unique_ptr<R::GxBuffer> buf, DWORD usage, DWORD fvfOrFormat,
	D3DPOOL pool)
	: GxNativeRefs(dev), m_dev(dev), m_buf(std::move(buf)), m_usage(usage), m_fvfOrFormat(fvfOrFormat), m_pool(pool)
{
	++m_dev.frontStats().liveBuffers;
	if (losable())
		++m_dev.frontStats().losable;
}

template <class I, class Desc>
GxNativeBuffer<I, Desc>::~GxNativeBuffer() = default;

template <class I, class Desc>
void GxNativeBuffer<I, Desc>::destroyFront()
{
	--m_dev.frontStats().liveBuffers;
	if (losable())
		--m_dev.frontStats().losable;
}

template <class I, class Desc>
void GxNativeBuffer<I, Desc>::destroyStorage()
{
	delete this;
}

template <class I, class Desc>
HRESULT GxNativeBuffer<I, Desc>::QueryInterface(REFIID riid, void** pp)
{
	if (!pp)
		return E_POINTER;
	if (isIID(riid, __uuidof(IUnknown)) || isIID(riid, __uuidof(IDirect3DResource8)) || isIID(riid, __uuidof(I))) {
		AddRef();
		*pp = static_cast<I*>(this);
		return S_OK;
	}
	*pp = nullptr;
	return E_NOINTERFACE;
}

template <class I, class Desc>
HRESULT GxNativeBuffer<I, Desc>::GetDevice(IDirect3DDevice8** pp)
{
	if (!pp)
		return D3DERR_INVALIDCALL;
	m_dev.AddRef();
	*pp = &m_dev;
	return D3D_OK;
}

template <class I, class Desc>
HRESULT GxNativeBuffer<I, Desc>::SetPrivateData(REFGUID, const void*, DWORD, DWORD) { return m_dev.neverCalled("Buffer::SetPrivateData"); }
template <class I, class Desc>
HRESULT GxNativeBuffer<I, Desc>::GetPrivateData(REFGUID, void*, DWORD*) { return noPrivateData(); }
template <class I, class Desc>
HRESULT GxNativeBuffer<I, Desc>::FreePrivateData(REFGUID) { return noPrivateData(); }

template <class I, class Desc>
D3DRESOURCETYPE GxNativeBuffer<I, Desc>::GetType()
{
	return std::is_same<I, IDirect3DVertexBuffer8>::value ? D3DRTYPE_VERTEXBUFFER : D3DRTYPE_INDEXBUFFER;
}

template <class I, class Desc>
HRESULT GxNativeBuffer<I, Desc>::Lock(UINT offset, UINT size, BYTE** data, DWORD flags)
{
	if (!data)
		return D3DERR_INVALIDCALL;
	if (m_dev.dbg().lockDropsDiscard && (flags & D3DLOCK_DISCARD))
		flags = (flags & ~DWORD(D3DLOCK_DISCARD)) | D3DLOCK_NOOVERWRITE;
	void* p = m_dev.lockBuffer(*m_buf, offset, size, flags);
	*data = static_cast<BYTE*>(p);
	return p ? D3D_OK : D3DERR_INVALIDCALL;
}

template <class I, class Desc>
HRESULT GxNativeBuffer<I, Desc>::Unlock()
{
	m_dev.unlockBuffer(*m_buf);
	return D3D_OK;
}

template <class I, class Desc>
HRESULT GxNativeBuffer<I, Desc>::GetDesc(Desc* d)
{
	if (!d)
		return D3DERR_INVALIDCALL;
	std::memset(d, 0, sizeof(*d));
	d->Usage = m_usage;
	d->Pool = m_pool;
	d->Size = m_buf->bytes();
	if constexpr (std::is_same<I, IDirect3DVertexBuffer8>::value) {
		d->Format = D3DFMT_VERTEXDATA;
		d->Type = D3DRTYPE_VERTEXBUFFER;
		d->FVF = m_fvfOrFormat;
	} else {
		d->Format = D3DFORMAT(m_fvfOrFormat);
		d->Type = D3DRTYPE_INDEXBUFFER;
	}
	return D3D_OK;
}

template class GxNativeBuffer<IDirect3DVertexBuffer8, D3DVERTEXBUFFER_DESC>;
template class GxNativeBuffer<IDirect3DIndexBuffer8, D3DINDEXBUFFER_DESC>;

}} // namespace GXRender::Dev
