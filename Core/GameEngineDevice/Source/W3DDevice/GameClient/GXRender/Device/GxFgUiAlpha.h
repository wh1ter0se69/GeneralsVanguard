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

// DLSS Frame Generation's UI alpha (shaders/gx_fg_uialpha.frag) - 1 where the
// final frame differs from the HUD-less one, both resolved by the same present shader, 0 elsewhere. One full-screen triangle into an
// R16_SFLOAT target at back-buffer size. Render-only; nothing here is read by the simulation.
#pragma once

#include "GxResources.h"
#include "VkCtx.h"

#include <string>
#include <vector>

namespace GXRender { namespace Dev {

class GxFgUiAlpha
{
public:
	// Streamline 2.14.1's Vulkan::getFormat (sl.chi vulkan.cpp) has no case for R8_UNORM: an R8 tag reads as UNDEFINED and logs
	// "Cannot have undefined format" on every describe (run H4: 120008 lines). R16_SFLOAT maps both ways and holds 0..1 exactly.
	static constexpr VkFormat kAlphaFormat = VK_FORMAT_R16_SFLOAT;

	// slots: descriptor sets, one per frame in flight (the caller waits for a slot's last submission before reusing it)
	bool init(Vk::VkCtx& ctx, uint32_t slots, std::string* why);
	void shutdown();
	// final + hudless: single-sample, same size; alpha: kAlphaFormat colour attachment, same size. Records outside a rendering scope; the two
	// inputs end in SHADER_READ_ONLY_OPTIMAL, alpha in SHADER_READ_ONLY_OPTIMAL (what DLSS-G reads at the present).
	bool record(VkCommandBuffer cmd, uint32_t slot, Res::GxImage& final, VkImageView finalView, Res::GxImage& hudless,
		VkImageView hudlessView, Res::GxImage& alpha, VkImageView alphaView);

private:
	Vk::VkCtx* m_ctx = nullptr;
	VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
	VkPipelineLayout m_layout = VK_NULL_HANDLE;
	VkSampler m_sampler = VK_NULL_HANDLE;
	VkDescriptorPool m_pool = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> m_sets;
	VkShaderModule m_vs = VK_NULL_HANDLE, m_fs = VK_NULL_HANDLE;
	VkPipeline m_pipeline = VK_NULL_HANDLE;
};

}} // namespace GXRender::Dev
