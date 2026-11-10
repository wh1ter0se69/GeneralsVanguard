// GeneralsX: WS-F1: image barriers for the pass graph (see GxImageTracker.h).
#include "GxImageTracker.h"

#include "GxResources.h"

#include <algorithm>

namespace GXRender { namespace Rec {

namespace {

const VkAccessFlags2 kWriteBits = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
	VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT |
	VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

VkAccessFlags2 expand(VkAccessFlags2 a)
{
	if (a & VK_ACCESS_2_MEMORY_READ_BIT)
		a |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT |
			VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT |
			VK_ACCESS_2_UNIFORM_READ_BIT | VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT;
	if (a & VK_ACCESS_2_MEMORY_WRITE_BIT)
		a |= kWriteBits;
	if (a & VK_ACCESS_2_SHADER_READ_BIT)
		a |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
	return a;
}

bool covers(VkPipelineStageFlags2 visStage, VkAccessFlags2 visAccess, VkPipelineStageFlags2 stage, VkAccessFlags2 access)
{
	const bool stageOk = (visStage & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) || (stage & ~visStage) == 0;
	return stageOk && (expand(access) & ~expand(visAccess)) == 0;
}

} // namespace

namespace Use {
GxImageUse colorAttachment(bool discard)
{
	return { VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true, discard };
}
GxImageUse depthAttachment(bool discard)
{
	return { VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
		VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, true, discard };
}
GxImageUse sampled()
{
	return { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
		false, false };
}
GxImageUse transferSrc()
{
	return { VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, false, false };
}
GxImageUse transferDst(bool discard)
{
	return { VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, true, discard };
}
GxImageUse resolveTarget(bool depth)
{
	if (depth)
		return { VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
				VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
			VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, true, true };
	return { VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, true, true };
}
} // namespace Use

GxImageTracker::State& GxImageTracker::state(Res::GxImage& img)
{
	State& s = m_states[&img];
	if (s.image != img.image || (s.layout != img.layout && !plantStaleLayout)) {
		if (s.image == img.image)
			++m_resyncs;   // someone else (GXRES) recorded a transition on an image we knew
		s = State();
		s.image = img.image;
		s.layout = img.layout;
		if (img.layout != VK_IMAGE_LAYOUT_UNDEFINED) {
			// an unknown write by anything, still to be made visible...
			s.writeStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			s.writeAccess = VK_ACCESS_2_MEMORY_WRITE_BIT;
			// ...except where GXRES' contract says what it did: copies -> SHADER_READ_ONLY, visible to sampling + transfer reads
			if (img.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
				s.visStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
				s.visAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
			}
		}
	}
	return s;
}

bool GxImageTracker::need(Res::GxImage& img, const GxImageUse& u)
{
	State& s = state(img);
	const bool layoutChange = u.layout != s.layout;
	bool hazard = false;
	if (s.writeAccess && !covers(s.visStage, s.visAccess, u.stage, u.access))
		hazard = true;   // read-after-write or write-after-write not yet ordered
	if (u.write && s.readStage)
		hazard = true;   // write-after-read: wait for the reads
	if (!layoutChange && !hazard) {
		if (u.write) {
			s.writeStage = u.stage;
			s.writeAccess = u.access & kWriteBits;
			s.visStage = s.visAccess = 0;
			s.readStage = 0;
		} else
			s.readStage |= u.stage;
		img.layout = s.layout;
		return false;
	}

	VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	b.srcStageMask = s.writeStage | s.readStage;
	b.srcAccessMask = s.writeAccess;
	if (plantSampledNoSrc && u.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
		s.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
		b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
		b.srcAccessMask = VK_ACCESS_2_NONE;
	}
	b.dstStageMask = u.stage;
	b.dstAccessMask = u.access;
	// a discard (CLEAR / DONT_CARE load) may drop only what the use covers: attachments are level 0, so an image with more
	// levels keeps them - the barrier spans every level and an UNDEFINED old layout would drop them all
	b.oldLayout = u.discard && img.levels <= 1 ? VK_IMAGE_LAYOUT_UNDEFINED : s.layout;
	b.newLayout = u.layout;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img.image;
	b.subresourceRange = { img.aspect(), 0, VK_REMAINING_MIP_LEVELS, 0, 1 };
	m_barriers.push_back(b);

	s.layout = u.layout;
	img.noteEntry();          // S7 A22: the layout this image entered the open frame with
	img.layout = u.layout;   // GXRES' view of the layout stays true (its own transitions read it)
	if (u.write) {
		s.writeStage = u.stage;
		s.writeAccess = u.access & kWriteBits;
		s.visStage = s.visAccess = 0;
		s.readStage = 0;
	} else {
		s.visStage |= u.stage;
		s.visAccess |= u.access;
		s.readStage = u.stage;
	}
	return true;
}

void GxImageTracker::flush(VkCommandBuffer cmd)
{
	if (m_barriers.empty())
		return;
	VkDependencyInfo d = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	d.imageMemoryBarrierCount = uint32_t(m_barriers.size());
	d.pImageMemoryBarriers = m_barriers.data();
	vkCmdPipelineBarrier2(cmd, &d);
	m_recorded += m_barriers.size();
	m_barriers.clear();
}

void GxImageTracker::unknownWrite(Res::GxImage& img)
{
	State& s = m_states[&img];
	s = State();
	s.image = img.image;
	s.layout = img.layout;
	s.writeStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	s.writeAccess = VK_ACCESS_2_MEMORY_WRITE_BIT;
}

void GxImageTracker::forget(const Res::GxImage* img)
{
	m_states.erase(img);
	// a queued barrier must not outlive the image: a never-submitted image can be destroyed (its handle freed) before flush()
	//
	if (img != nullptr)
		m_barriers.erase(std::remove_if(m_barriers.begin(), m_barriers.end(),
			[img](const VkImageMemoryBarrier2& b) { return b.image == img->image; }), m_barriers.end());
}

}} // namespace GXRender::Rec
