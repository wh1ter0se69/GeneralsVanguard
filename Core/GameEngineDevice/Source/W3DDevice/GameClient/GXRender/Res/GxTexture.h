// GeneralsX: WS-C GXRES v0: 2D textures and standalone surfaces with D3D8 semantics.
//
// MANAGED   image + a host copy of every level (DXVK keeps the same copy for this pool: d3d9_common_texture.cpp
//           DetermineMapMode -> BACKED, never evicted on 64-bit). LockRect returns the host copy, so texels a lock does not
//           write keep their values; UnlockRect uploads only the locked rectangle. dropHostCopy() is upload-then-drop: the
//           host copy goes, and a later lock first reads the level back from the GPU (counted: restoresAfterDrop). The
//           census (CENSUS §3) says which textures may drop safely; this class only provides the mechanism.
// SYSTEMMEM host memory only, with a stable pointer for the object's lifetime (W3DShroud writes through pBits kept after
//           UnlockRect). Copied to the GPU only by copyFrom() (CopyRects / UpdateTexture), which reads it at call time.
// DEFAULT   image only; not lockable (D3D8). Filled by copyFrom() or rendered into (usage RENDERTARGET).
//
// DECODED  (rw2-C) a DXTn texture on a device that cannot sample BC (FormatTable::decodes): the host copy keeps the blocks, so
//           LockRect is unchanged for the game, and each uploaded rect is decoded on the CPU into an A8R8G8B8 image. Its
//           host copy is never dropped (a restore cannot re-encode), readLevel returns the decoded texels.
// One image per texture, whole-image layout tracking (GxImage). Cube and volume textures: none in a G2 run (CENSUS §3a) - v1.
#pragma once
#include "GxResources.h"
#include <vector>

namespace GXRender { namespace Res {

struct TextureDesc
{
	uint32_t width = 0, height = 0;
	uint32_t levels = 1;   // 0 = the full chain
	uint32_t format = 0;   // d3d:: format
	uint32_t usage = 0;    // d3d::USAGE_* (RENDERTARGET makes it an attachment too)
	uint32_t pool = 0;     // d3d::POOL_*
};

struct LockedRect
{
	void* bits = nullptr;
	uint32_t pitch = 0;    // bytes between rows (block rows for BC)
};

class GxTexture
{
public:
	GxTexture() = default;
	~GxTexture() { destroy(); }
	GxTexture(const GxTexture&) = delete;
	GxTexture& operator=(const GxTexture&) = delete;

	bool init(GxResources& res, const TextureDesc& desc, std::string* why);
	void destroy();

	// rect null = the whole level; block formats need block-aligned rects (D3D rule). DEFAULT textures refuse.
	bool lockRect(uint32_t level, const VkRect2D* rect, uint32_t flags, LockedRect* out);
	void unlockRect(uint32_t level);
	// A23-B §3.6 (the native device's worker): unlockRect split. FRONT: closes the lock; true = a MANAGED written level whose
	// rect the worker uploads - copy its rows (uploadBytes / rowPitch) from hostLevel() into the command. WORKER: stages them.
	bool unlockFront(uint32_t level, VkRect2D* rect);
	void execUploadRect(uint32_t level, const VkRect2D& r, const uint8_t* src, uint32_t srcPitch) { stageRect(level, r, src, srcPitch); }
	// The rect's rows as stageRect reads them from the host copy: tight row bytes, block rows, and the staging bytes it hands out
	// (decoded: w x h x 4 - the IDLE-FLUSH bound of §3.6).
	uint32_t rectRowBytes(const VkRect2D& r) const;
	uint32_t rectRows(const VkRect2D& r) const;
	uint64_t rectStagingBytes(const VkRect2D& r) const;
	const uint8_t* rectHostStart(uint32_t level, const VkRect2D& r) const;
	bool rectValid(uint32_t level, const VkRect2D& r) const { return m_res && level < m_levels && validRect(level, r); }

	// Sampled view (format swizzle, SetLOD applied); records that submission useValue reads the texture.
	VkImageView view(uint64_t useValue);
	// Identity-swizzle single-level view for rendering into a RENDERTARGET texture.
	VkImageView attachmentView();
	// D3D SetLOD (MANAGED): the most detailed level sampled from now on. Returns the previous value.
	uint32_t setLod(uint32_t lod);

	bool dropHostCopy();          // MANAGED, no open lock, not decoded; false otherwise
	bool hasHostCopy(uint32_t level) const;
	const uint8_t* hostLevel(uint32_t level) const;   // the host copy (MANAGED / SYSTEMMEM), tight pitch

	// GPU -> host read of one level (tight rows). Tests, GX_SHOT-style read-backs, and the restore of a dropped level.
	bool readLevel(uint32_t level, std::vector<uint8_t>* out);

	// CopyRects / UpdateTexture: same format. Host -> GPU when src is SYSTEMMEM/MANAGED and this texture has an image
	// (staged now, uploaded by the next recordPending); GPU -> host when src has only an image and this is SYSTEMMEM
	// (synchronous read-back). A MANAGED destination keeps its host copy in step.
	bool copyFrom(GxTexture& src, uint32_t srcLevel, const VkRect2D& srcRect, uint32_t dstLevel, VkOffset2D dstPoint);

	GxImage& image() { return m_image; }
	bool decoded() const { return m_decoded; }  // DXTn uploaded as decoded A8R8G8B8 (no BC on this device)
	const TextureDesc& desc() const { return m_desc; }
	const FormatInfo* format() const { return m_format; }
	uint32_t levels() const { return m_levels; }
	uint32_t levelWidth(uint32_t level) const { return m_desc.width >> level ? m_desc.width >> level : 1; }
	uint32_t levelHeight(uint32_t level) const { return m_desc.height >> level ? m_desc.height >> level : 1; }
	uint32_t levelPitch(uint32_t level) const;

private:
	bool validRect(uint32_t level, const VkRect2D& r) const;
	bool restoreLevel(uint32_t level);
	void uploadRect(uint32_t level, const VkRect2D& r);
	// Stages rect r of `level` from src (the rect's first row of blocks/texels, srcPitch apart) and queues its copy; a decoded
	// texture stages the rect decoded to A8R8G8B8.
	bool stageRect(uint32_t level, const VkRect2D& r, const uint8_t* src, uint32_t srcPitch);

	GxResources* m_res = nullptr;
	TextureDesc m_desc;
	const FormatInfo* m_format = nullptr;
	uint32_t m_levels = 1;
	bool m_decoded = false;
	GxImage m_image;
	std::vector<std::vector<uint8_t>> m_host;     // per level; empty = none (DEFAULT, or dropped)
	std::vector<VkRect2D> m_lockRect;              // per level
	std::vector<uint32_t> m_lockFlags;
	std::vector<bool> m_locked;
	uint32_t m_lod = 0;
	std::vector<VkImageView> m_views;              // per lod, created on demand
	VkImageView m_attachmentView = VK_NULL_HANDLE;
};

}} // namespace GXRender::Res
