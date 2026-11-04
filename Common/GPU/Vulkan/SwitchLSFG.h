#pragma once

#include <vulkan/vulkan_core.h>

void SwitchLSFG_Configure(bool prepared, float flowScale, bool performanceMode, bool deko3d = false);
// Published by emulation frame timing; render threads consume this atomically.
void SwitchLSFG_SetFastForward(bool fastForward);

struct SwitchLSFGDekoState {
	bool enabled = false, active = false, resetSession = false;
	uint32_t speedControlState = 0;
	float flowScale = 0.25f;
	bool performance = true;
};
SwitchLSFGDekoState SwitchLSFG_PollDeko();
void SwitchLSFG_SetDekoStatus(bool available, bool highRate);

bool SwitchLSFG_PrepareDeviceFeatures(VkPhysicalDevice physicalDevice, void*& featureChain,
	VkPhysicalDeviceTimelineSemaphoreFeatures& timelineFeature);
void SwitchLSFG_SetDevice(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
	VkQueue queue, uint32_t queueFamily, PFN_vkGetInstanceProcAddr getInstanceProcAddr);
void SwitchLSFG_ResetDevice();

bool SwitchLSFG_AdjustSwapchainCreateInfo(bool mainWindow, VkSwapchainCreateInfoKHR& createInfo,
	const VkSurfaceCapabilitiesKHR& capabilities);
void SwitchLSFG_AttachSwapchain(bool mainWindow, VkSwapchainKHR swapchain, VkExtent2D extent,
	const VkImage* images, uint32_t imageCount, bool compatible);
void SwitchLSFG_DetachSwapchain(VkSwapchainKHR swapchain);

bool SwitchLSFG_Present(VkQueue queue, const VkPresentInfoKHR& presentInfo, VkResult& result);

bool SwitchLSFG_IsPrepared();
bool SwitchLSFG_IsInstalled();
bool SwitchLSFG_IsAvailable();
bool SwitchLSFG_IsEnabled();
bool SwitchLSFG_IsHighRatePassthrough();
bool SwitchLSFG_RequestEnabled(bool enabled, float sourceFPS);
void SwitchLSFG_ResetSession();
