// GeneralsX: the one translation unit that compiles VMA's implementation. Entry points come
// from volk through VmaVulkanFunctions (VkCtx::createDevice), never from a linked loader.
#include "volk.h"

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
