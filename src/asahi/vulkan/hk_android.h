#pragma once

#include "vulkan/vulkan_core.h"

struct hk_device;

#ifdef VK_USE_PLATFORM_ANDROID_KHR

bool hk_android_is_gralloc_image(const VkImageCreateInfo *pCreateInfo);

VkResult hk_android_create_gralloc_image(
   VkDevice device, const VkImageCreateInfo *pCreateInfo,
   const VkAllocationCallbacks *pAllocator, VkImage *pImage);

VkResult hk_android_get_wsi_memory(struct hk_device *dev,
                                   const VkBindImageMemoryInfo *bind_info,
                                   VkDeviceMemory *out_mem_handle);

#else

static inline bool
hk_android_is_gralloc_image(const VkImageCreateInfo *pCreateInfo)
{
   return false;
}

static inline VkResult
hk_android_create_gralloc_image(VkDevice device,
                                const VkImageCreateInfo *pCreateInfo,
                                const VkAllocationCallbacks *pAllocator,
                                VkImage *pImage)
{
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

static inline VkResult
hk_android_get_wsi_memory(struct hk_device *dev,
                          const VkBindImageMemoryInfo *bind_info,
                          VkDeviceMemory *out_mem_handle)
{
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

#endif
