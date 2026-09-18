/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PPSSPP_SWITCH_LSFG_BRIDGE_H
#define PPSSPP_SWITCH_LSFG_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LsfgNxRuntime LsfgNxRuntime;

typedef struct LsfgNxCreateInfo {
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  VkQueue queue;
  uint32_t queue_family_index;
  PFN_vkGetInstanceProcAddr get_instance_proc_addr;

  VkSwapchainKHR swapchain;
  VkExtent2D extent;
  const VkImage *swapchain_images;
  uint32_t swapchain_image_count;

  const char *shader_dll_path;
  float flow_scale;
  bool performance_mode;
} LsfgNxCreateInfo;

LsfgNxRuntime *lsfg_nx_create(const LsfgNxCreateInfo *info);
void lsfg_nx_destroy(LsfgNxRuntime *runtime);

bool lsfg_nx_present(LsfgNxRuntime *runtime, VkQueue queue,
                     const VkPresentInfoKHR *present_info, VkResult *result);

#ifdef __cplusplus
}
#endif

#endif /* PPSSPP_SWITCH_LSFG_BRIDGE_H */
