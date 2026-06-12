#include "hk_android.h"

#ifdef VK_USE_PLATFORM_ANDROID_KHR

#include "hk_device.h"
#include "hk_device_memory.h"
#include "hk_image.h"

#include "util/macros.h"
#include "util/os_file.h"
#include "vk_alloc.h"
#include "vk_android.h"
#include "vk_util.h"

#include "vulkan/vk_android_native_buffer.h"

#include <errno.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

bool
hk_android_is_gralloc_image(const VkImageCreateInfo *pCreateInfo)
{
   vk_foreach_struct_const(ext, pCreateInfo->pNext) {
      switch ((uint32_t)ext->sType) {
      case VK_STRUCTURE_TYPE_NATIVE_BUFFER_ANDROID:
         return true;
      case VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR: {
         const VkImageSwapchainCreateInfoKHR *swapchain_info = (void *)ext;
         if (swapchain_info->swapchain != VK_NULL_HANDLE)
            return true;
         break;
      }
      default:
         break;
      }
   }

   return false;
}

struct hk_android_deferred_image {
   struct hk_image base;

   VkImageCreateInfo *create_info;
   bool initialized;
};

static VkResult
hk_android_create_deferred_image(VkDevice device,
                                 const VkImageCreateInfo *pCreateInfo,
                                 const VkAllocationCallbacks *pAllocator,
                                 VkImage *pImage)
{
   VK_FROM_HANDLE(hk_device, dev, device);

   uint32_t queue_family_count = 0;
   uint32_t view_format_count = 0;

   if (pCreateInfo->sharingMode == VK_SHARING_MODE_CONCURRENT)
      queue_family_count = pCreateInfo->queueFamilyIndexCount;

   const VkImageFormatListCreateInfo *raw_list =
      vk_find_struct_const(pCreateInfo->pNext, IMAGE_FORMAT_LIST_CREATE_INFO);
   if (raw_list)
      view_format_count = raw_list->viewFormatCount;

   VK_MULTIALLOC(ma);
   VK_MULTIALLOC_DECL(&ma, struct hk_android_deferred_image, deferred, 1);
   VK_MULTIALLOC_DECL(&ma, VkImageCreateInfo, create_info, 1);
   VK_MULTIALLOC_DECL(&ma, VkImageFormatListCreateInfo, list_info, 1);
   VK_MULTIALLOC_DECL(&ma, VkImageStencilUsageCreateInfo, stencil_info, 1);
   VK_MULTIALLOC_DECL(&ma, uint32_t, queue_families, queue_family_count);
   VK_MULTIALLOC_DECL(&ma, uint32_t, view_formats, view_format_count);

   if (!vk_multialloc_zalloc2(&ma, &dev->vk.alloc, pAllocator,
                              VK_SYSTEM_ALLOCATION_SCOPE_OBJECT))
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_image_init(&dev->vk, &deferred->base.vk, pCreateInfo);

   *create_info = *pCreateInfo;
   create_info->pNext = NULL;
   create_info->format = deferred->base.vk.format;
   create_info->tiling = deferred->base.vk.tiling =
      VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;

   if (queue_family_count) {
      memcpy(queue_families, pCreateInfo->pQueueFamilyIndices,
             sizeof(*queue_families) * queue_family_count);
      create_info->pQueueFamilyIndices = queue_families;
   }

   if (view_format_count ||
       (deferred->base.vk.create_flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) {
      if (view_format_count) {
         memcpy(view_formats, raw_list->pViewFormats,
                sizeof(*view_formats) * view_format_count);
      } else {
         view_format_count = 1;
         view_formats = &create_info->format;
      }

      *list_info = (VkImageFormatListCreateInfo){
         .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
         .viewFormatCount = view_format_count,
         .pViewFormats = view_formats,
      };
      __vk_append_struct(create_info, list_info);
   }

   if (deferred->base.vk.stencil_usage) {
      *stencil_info = (VkImageStencilUsageCreateInfo){
         .sType = VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO,
         .stencilUsage = deferred->base.vk.stencil_usage,
      };
      __vk_append_struct(create_info, stencil_info);
   }

   deferred->create_info = create_info;
   *pImage = hk_image_to_handle(&deferred->base);

   return VK_SUCCESS;
}

static uint32_t
hk_android_get_fd_mem_type_bits(VkDevice dev_handle, int dma_buf_fd)
{
   VK_FROM_HANDLE(hk_device, dev, dev_handle);

   VkMemoryFdPropertiesKHR fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
   };
   VkResult result = dev->vk.dispatch_table.GetMemoryFdPropertiesKHR(
      dev_handle, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dma_buf_fd,
      &fd_props);

   return result == VK_SUCCESS ? fd_props.memoryTypeBits : 0;
}

static VkResult
hk_android_get_image_mem_reqs(VkDevice dev_handle, VkImage img_handle,
                              int dma_buf_fd,
                              VkMemoryRequirements *out_mem_reqs)
{
   VK_FROM_HANDLE(hk_device, dev, dev_handle);
   VkMemoryRequirements mem_reqs;

   dev->vk.dispatch_table.GetImageMemoryRequirements(dev_handle, img_handle,
                                                     &mem_reqs);

   const uint32_t fd_mem_type_bits =
      hk_android_get_fd_mem_type_bits(dev_handle, dma_buf_fd);
   if (!(mem_reqs.memoryTypeBits & fd_mem_type_bits)) {
      return vk_errorf(dev, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                       "No compatible mem type: img req (%u), fd req (%u)",
                       mem_reqs.memoryTypeBits, fd_mem_type_bits);
   }

   mem_reqs.memoryTypeBits &= fd_mem_type_bits;
   *out_mem_reqs = mem_reqs;

   return VK_SUCCESS;
}

static VkResult
hk_android_import_anb_memory(VkDevice dev_handle, VkImage img_handle,
                             const VkNativeBufferANDROID *anb,
                             const VkAllocationCallbacks *alloc)
{
   VK_FROM_HANDLE(hk_device, dev, dev_handle);
   VK_FROM_HANDLE(hk_image, img, img_handle);
   VkMemoryRequirements mem_reqs;
   VkResult result;

   assert(anb && anb->handle && anb->handle->numFds > 0);

   int dma_buf_fd = anb->handle->data[0];
   result =
      hk_android_get_image_mem_reqs(dev_handle, img_handle, dma_buf_fd,
                                    &mem_reqs);
   if (result != VK_SUCCESS)
      return result;

   int dup_fd = os_dupfd_cloexec(dma_buf_fd);
   if (dup_fd < 0) {
      return vk_error(dev, errno == EMFILE ? VK_ERROR_TOO_MANY_OBJECTS
                                           : VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   const VkMemoryDedicatedAllocateInfo dedicated_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = img_handle,
   };
   const VkImportMemoryFdInfoKHR fd_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = &dedicated_info,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = dup_fd,
   };
   const VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &fd_info,
      .allocationSize = mem_reqs.size,
      .memoryTypeIndex = ffs(mem_reqs.memoryTypeBits) - 1,
   };

   result = dev->vk.dispatch_table.AllocateMemory(
      dev_handle, &alloc_info, alloc, &img->vk.anb_memory);
   if (result != VK_SUCCESS) {
      close(dup_fd);
      return result;
   }

   return VK_SUCCESS;
}

static VkResult
hk_android_anb_init(struct hk_device *dev, VkImageCreateInfo *create_info,
                    const VkNativeBufferANDROID *anb,
                    const VkAllocationCallbacks *alloc, struct hk_image *img,
                    bool *out_initialized)
{
   VkResult result;

   if (out_initialized)
      *out_initialized = false;

   VkImageDrmFormatModifierExplicitCreateInfoEXT mod_info;
   VkSubresourceLayout layouts[ARRAY_SIZE(img->planes)];
   assert(vk_find_struct_const(create_info->pNext, NATIVE_BUFFER_ANDROID));
   result = vk_android_get_anb_layout(create_info, &mod_info, layouts,
                                      ARRAY_SIZE(img->planes));
   if (result != VK_SUCCESS)
      return result;

   mod_info.pNext = create_info->pNext;
   const VkExternalMemoryImageCreateInfo external_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &mod_info,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };

   create_info->pNext = &external_info;
   result = hk_image_init(dev, img, create_info);
   if (result != VK_SUCCESS)
      return result;

   if (out_initialized)
      *out_initialized = true;

   result = hk_image_alloc_vmas(dev, img);
   if (result != VK_SUCCESS)
      return result;

   return hk_android_import_anb_memory(hk_device_to_handle(dev),
                                       hk_image_to_handle(img), anb, alloc);
}

VkResult
hk_android_create_gralloc_image(VkDevice device,
                                const VkImageCreateInfo *pCreateInfo,
                                const VkAllocationCallbacks *pAllocator,
                                VkImage *pImage)
{
   VK_FROM_HANDLE(hk_device, dev, device);
   VkResult result;

   const VkNativeBufferANDROID *anb =
      vk_find_struct_const(pCreateInfo->pNext, NATIVE_BUFFER_ANDROID);
   if (!anb)
      return hk_android_create_deferred_image(device, pCreateInfo, pAllocator,
                                              pImage);

   struct hk_image *img =
      vk_zalloc2(&dev->vk.alloc, pAllocator, sizeof(*img), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!img)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkImageCreateInfo create_info = *pCreateInfo;
   create_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;

   bool initialized = false;
   result =
      hk_android_anb_init(dev, &create_info, anb, pAllocator, img,
                          &initialized);
   if (result != VK_SUCCESS) {
      if (initialized)
         hk_image_finish(dev, img, pAllocator);
      vk_free2(&dev->vk.alloc, pAllocator, img);
      return result;
   }

   VkImage img_handle = hk_image_to_handle(img);
   result = dev->vk.dispatch_table.BindImageMemory(device, img_handle,
                                                   img->vk.anb_memory, 0);
   if (result != VK_SUCCESS) {
      hk_image_finish(dev, img, pAllocator);
      vk_free2(&dev->vk.alloc, pAllocator, img);
      return result;
   }

   *pImage = img_handle;
   return VK_SUCCESS;
}

VkResult
hk_android_get_wsi_memory(struct hk_device *dev,
                          const VkBindImageMemoryInfo *bind_info,
                          VkDeviceMemory *out_mem_handle)
{
   VK_FROM_HANDLE(hk_image, img, bind_info->image);

   const VkNativeBufferANDROID *anb =
      vk_find_struct_const(bind_info->pNext, NATIVE_BUFFER_ANDROID);
   if (!anb)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   struct hk_android_deferred_image *deferred =
      container_of(img, struct hk_android_deferred_image, base);
   assert(deferred->create_info && !deferred->initialized);

   VkNativeBufferANDROID local_anb = *anb;
   const void *saved_pNext = deferred->create_info->pNext;
   local_anb.pNext = saved_pNext;
   deferred->create_info->pNext = &local_anb;

   VkResult result =
      hk_android_anb_init(dev, deferred->create_info, anb, &dev->vk.alloc, img,
                          NULL);
   deferred->create_info->pNext = saved_pNext;
   if (result != VK_SUCCESS)
      return result;

   deferred->initialized = true;
   *out_mem_handle = img->vk.anb_memory;

   return VK_SUCCESS;
}

#endif
