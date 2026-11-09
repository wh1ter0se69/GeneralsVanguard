// GeneralsX @feature E · rw2-F1 11/10/2026 WS-F1: per-image access state and the precise barriers the pass graph needs.
//
// GXRES' own GxImage::transition is a full barrier (everything -> one use): right for its rare copies, too blunt for a frame
// that hands a render target to its readers. The writer asks need(image, use) before every attachment, sampled or transfer
// use; the tracker queues a barrier only when the use is not already covered (a layout change, a write not yet visible to
// that stage/access, or a write after reads), and flush() records the queued ones in one vkCmdPipelineBarrier2 - always
// OUTSIDE a rendering scope (dynamic rendering allows no such barrier inside).
//
// State survives command buffers (a write in frame N still needs a barrier before a read in frame N+1: queue order is an
// execution order, not a memory dependency). When something else changed the image (its GxImage::layout or VkImage differs
// from what the tracker last saw - GXRES recordPending / resolveTo / readImage), the tracker assumes an unknown write, except
// for the GXRES contract case (GXRES_v0.md "The contract" #2): an image GXRES left in SHADER_READ_ONLY_OPTIMAL has its copy
// writes already visible to sampling and transfer reads at every stage.
#pragma once

#include "VkCommon.h"

#include <unordered_map>
#include <vector>

namespace GXRender { namespace Res { class GxImage; } }

namespace GXRender { namespace Rec {

struct GxImageUse
{
	VkImageLayout layout;
	VkPipelineStageFlags2 stage;
	VkAccessFlags2 access;
	bool write;
	bool discard;   // the previous contents do not matter (CLEAR / DONT_CARE load, whole-image overwrite)
};

// The uses the writer makes of an image.
namespace Use {
GxImageUse colorAttachment(bool discard);
GxImageUse depthAttachment(bool discard);
GxImageUse sampled();
GxImageUse transferSrc();
GxImageUse transferDst(bool discard);
// F-depth+ (D · look-fdepth-plus): the single-sample target of a render-pass multisample resolve. The spec runs every resolve,
// depth/stencil too, in COLOR_ATTACHMENT_OUTPUT with COLOR_ATTACHMENT_WRITE; a depth target also names the fragment-test stages
// and depth write (a superset: right under either reading). Always a discard - the resolve writes the whole render area.
GxImageUse resolveTarget(bool depth);
} // namespace Use

class GxImageTracker
{
public:
	// TEST-ONLY plant (the missing-barrier fail arm): an attachment -> sampled hand-off records its layout change with an
	// EMPTY source scope, i.e. without waiting for the attachment writes. Synchronization validation must report it.
	bool plantSampledNoSrc = false;
	// TEST-ONLY plant (D · rw3-T-4, X-hoist-stalelayout): state() ignores a layout someone else changed (a hoisted copy's in the
	// init buffer) and keeps its own - the next barrier names a stale old layout. Validation must report it.
	bool plantStaleLayout = false;

	// Queues the barrier img needs before u; false when the current state already covers u (nothing queued).
	bool need(Res::GxImage& img, const GxImageUse& u);
	bool pending() const { return !m_barriers.empty(); }
	void flush(VkCommandBuffer cmd);          // records every queued barrier (one call); no-op when none
	void forget(const Res::GxImage* img);     // the image is being destroyed
	// Something outside the tracker wrote img (outsideScope work): an unknown write in img's CURRENT layout, made visible to
	// nothing - the next use gets a full barrier whatever the layout says
	void unknownWrite(Res::GxImage& img);
	uint64_t barriersRecorded() const { return m_recorded; }
	const std::vector<VkImageMemoryBarrier2>& queued() const { return m_barriers; }   // tests: the barriers flush() would record
	uint64_t externalResyncs() const { return m_resyncs; }

private:
	struct State
	{
		VkImage image = VK_NULL_HANDLE;
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkPipelineStageFlags2 writeStage = 0;   // the last write (0 = none since the image was created)
		VkAccessFlags2 writeAccess = 0;
		VkPipelineStageFlags2 visStage = 0;     // where that write has been made visible
		VkAccessFlags2 visAccess = 0;
		VkPipelineStageFlags2 readStage = 0;    // reads since the last write (a later write must wait for them)
	};
	State& state(Res::GxImage& img);

	std::unordered_map<const Res::GxImage*, State> m_states;
	std::vector<VkImageMemoryBarrier2> m_barriers;
	uint64_t m_recorded = 0;
	uint64_t m_resyncs = 0;
};

}} // namespace GXRender::Rec
