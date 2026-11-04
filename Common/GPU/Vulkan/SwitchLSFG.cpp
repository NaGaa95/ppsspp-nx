#include "Common/GPU/Vulkan/SwitchLSFG.h"
#include "Core/Config.h"
#include "Common/System/System.h"
#include "Core/System.h"

#include "Common/GPU/Vulkan/VulkanLoader.h"
#include "Common/GPU/Vulkan/lsfg/lsfg_bridge.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
constexpr const char* kShaderDllPath = "/switch/ppsspp-nx/lsfg/Lossless.dll";

std::atomic_bool s_prepared{false};
std::atomic_bool s_installed{false};
std::atomic_bool s_available{false};
std::atomic_bool s_enabledRequested{false};
std::atomic_int s_sessionOverride{-1};
std::atomic_bool s_highRatePassthrough{false};
std::atomic_bool s_resetSessionRequested{false};
std::atomic_bool s_deko3d{false};
// Bit 0 is fast-forward; the remaining bits count speed-control transitions.
std::atomic_uint32_t s_speedControlState{0};
uint32_t s_observedSpeedControlState = 0;

float s_flowScale = 0.25f;
bool s_performanceMode = true;
bool s_deviceFeatureEnabled = false;
VkInstance s_instance = VK_NULL_HANDLE;
VkPhysicalDevice s_physicalDevice = VK_NULL_HANDLE;
VkDevice s_device = VK_NULL_HANDLE;
VkQueue s_queue = VK_NULL_HANDLE;
uint32_t s_queueFamily = VK_QUEUE_FAMILY_IGNORED;
PFN_vkGetInstanceProcAddr s_getInstanceProcAddr = nullptr;

VkSwapchainKHR s_swapchain = VK_NULL_HANDLE;
VkExtent2D s_extent{};
std::vector<VkImage> s_swapchainImages;
bool s_swapchainCompatible = false;
LsfgNxRuntime* s_runtime = nullptr;
bool s_initAttempted = false;

uint64_t s_lastSourcePresentNs = 0;
double s_sourceIntervalNs = 0.0;
unsigned s_sourceSamples = 0;
unsigned s_highRateSlowSamples = 0;
bool s_previousRequested = false;
int s_rateDecision = -1;

bool FileReadable(const char* path)
{
	FILE* file = std::fopen(path, "rb");
	if (!file)
		return false;
	std::fclose(file);
	return true;
}

bool EnableNvkNoCbuf()
{
	const char* current = std::getenv("NVK_DEBUG");
	if (current && std::strstr(current, "no_cbuf"))
		return true;
	const std::string value = current && *current ? std::string(current) + ",no_cbuf" : "no_cbuf";
	return setenv("NVK_DEBUG", value.c_str(), 1) == 0;
}

void DestroyRuntime()
{
	if (!s_runtime)
		return;
	lsfg_nx_destroy(s_runtime);
	s_runtime = nullptr;
}

void ResetTiming()
{
	s_lastSourcePresentNs = 0;
	s_sourceIntervalNs = 0.0;
	s_sourceSamples = 0;
	s_highRateSlowSamples = 0;
	s_previousRequested = false;
	s_rateDecision = -1;
	s_highRatePassthrough.store(false, std::memory_order_release);
}

void ResetSwapchain()
{
	DestroyRuntime();
	s_swapchain = VK_NULL_HANDLE;
	s_extent = {};
	s_swapchainImages.clear();
	s_swapchainCompatible = false;
	s_initAttempted = false;
	s_observedSpeedControlState = 0;
	s_available.store(false, std::memory_order_release);
	ResetTiming();
}

uint64_t MonotonicNs()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t ObserveSourcePresent()
{
	const uint64_t now = MonotonicNs();
	uint64_t interval = 0;
	if (s_lastSourcePresentNs && now > s_lastSourcePresentNs)
	{
		interval = now - s_lastSourcePresentNs;
		if (interval >= 4'000'000 && interval <= 100'000'000)
		{
			s_sourceIntervalNs = s_sourceSamples == 0 ? static_cast<double>(interval) :
				s_sourceIntervalNs * 0.875 + static_cast<double>(interval) * 0.125;
			if (s_sourceSamples < 120)
				++s_sourceSamples;
		}
		else
			interval = 0;
	}
	s_lastSourcePresentNs = now;
	return interval;
}

bool SourceIsHighRate()
{
	return s_sourceSamples >= 8 && s_sourceIntervalNs > 0.0 && s_sourceIntervalNs < 30'000'000.0;
}

bool TryCreateRuntime(VkQueue queue)
{
	if (s_runtime)
		return true;
	if (s_initAttempted || !s_swapchainCompatible || queue != s_queue ||
		!s_instance || !s_physicalDevice || !s_device || !s_getInstanceProcAddr ||
		s_swapchainImages.size() < 3)
		return false;

	s_initAttempted = true;
	LsfgNxCreateInfo info{
		.instance = s_instance,
		.physical_device = s_physicalDevice,
		.device = s_device,
		.queue = s_queue,
		.queue_family_index = s_queueFamily,
		.get_instance_proc_addr = s_getInstanceProcAddr,
		.swapchain = s_swapchain,
		.extent = s_extent,
		.swapchain_images = s_swapchainImages.data(),
		.swapchain_image_count = static_cast<uint32_t>(s_swapchainImages.size()),
		.shader_dll_path = kShaderDllPath,
		.flow_scale = s_flowScale,
		.performance_mode = s_performanceMode,
	};
	s_runtime = lsfg_nx_create(&info);
	return s_runtime != nullptr;
}
}

void SwitchLSFG_Configure(bool prepared, float flowScale, bool performanceMode, bool deko3d) {
	s_flowScale = std::isfinite(flowScale) && flowScale == 0.5f ? 0.5f : 0.25f;
	s_performanceMode = performanceMode;
	const bool installed = FileReadable(kShaderDllPath);
	s_installed.store(installed, std::memory_order_release);
	s_deko3d.store(deko3d, std::memory_order_release);
	prepared = prepared && installed && (deko3d || EnableNvkNoCbuf());
	s_prepared.store(prepared, std::memory_order_release);
	s_sessionOverride.store(-1, std::memory_order_release);
	s_enabledRequested.store(false, std::memory_order_release);
	s_available.store(deko3d && prepared, std::memory_order_release);
	s_highRatePassthrough.store(false, std::memory_order_release);
	s_resetSessionRequested.store(false, std::memory_order_release);
	s_speedControlState.store(0, std::memory_order_release);
}

void SwitchLSFG_SetFastForward(bool fastForward) {
	uint32_t state = s_speedControlState.load(std::memory_order_relaxed);
	while (((state & 1) != 0) != fastForward) {
		const uint32_t next = ((state + 2) & ~1u) | (fastForward ? 1u : 0u);
		if (s_speedControlState.compare_exchange_weak(state, next, std::memory_order_release, std::memory_order_relaxed)) {
			break;
		}
	}
}

bool SwitchLSFG_PrepareDeviceFeatures(VkPhysicalDevice physicalDevice, void*& featureChain,
	VkPhysicalDeviceTimelineSemaphoreFeatures& timelineFeature)
{
	s_deviceFeatureEnabled = false;
	if (!s_prepared.load(std::memory_order_acquire) || s_deko3d.load(std::memory_order_acquire)) {
		return false;
	}

	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(physicalDevice, &properties);
	if (properties.apiVersion < VK_API_VERSION_1_2)
		return false;

	VkPhysicalDeviceTimelineSemaphoreFeatures supported{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
	};
	VkPhysicalDeviceFeatures2 features{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &supported,
	};
	vkGetPhysicalDeviceFeatures2(physicalDevice, &features);
	if (!supported.timelineSemaphore)
		return false;

	timelineFeature = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
		.pNext = featureChain,
		.timelineSemaphore = VK_TRUE,
	};
	featureChain = &timelineFeature;
	s_deviceFeatureEnabled = true;
	return true;
}

void SwitchLSFG_SetDevice(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
	VkQueue queue, uint32_t queueFamily, PFN_vkGetInstanceProcAddr getInstanceProcAddr)
{
	s_instance = instance;
	s_physicalDevice = physicalDevice;
	s_device = device;
	s_queue = queue;
	s_queueFamily = queueFamily;
	s_getInstanceProcAddr = getInstanceProcAddr;
	if (!s_deviceFeatureEnabled)
		s_available.store(false, std::memory_order_release);
}

void SwitchLSFG_ResetDevice()
{
	ResetSwapchain();
	s_instance = VK_NULL_HANDLE;
	s_physicalDevice = VK_NULL_HANDLE;
	s_device = VK_NULL_HANDLE;
	s_queue = VK_NULL_HANDLE;
	s_queueFamily = VK_QUEUE_FAMILY_IGNORED;
	s_getInstanceProcAddr = nullptr;
	s_deviceFeatureEnabled = false;
	s_enabledRequested.store(false, std::memory_order_release);
}

bool SwitchLSFG_AdjustSwapchainCreateInfo(bool mainWindow, VkSwapchainCreateInfoKHR& createInfo,
	const VkSurfaceCapabilitiesKHR& capabilities)
{
	if (!mainWindow || !s_prepared.load(std::memory_order_acquire) || !s_deviceFeatureEnabled ||
		!s_installed.load(std::memory_order_acquire))
		return false;

	constexpr VkImageUsageFlags transferUsage =
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if ((capabilities.supportedUsageFlags & transferUsage) != transferUsage)
		return false;

	VkFormatProperties swapchainProperties{};
	VkFormatProperties rgbaProperties{};
	vkGetPhysicalDeviceFormatProperties(s_physicalDevice, createInfo.imageFormat, &swapchainProperties);
	vkGetPhysicalDeviceFormatProperties(s_physicalDevice, VK_FORMAT_R8G8B8A8_UNORM, &rgbaProperties);
	constexpr VkFormatFeatureFlags copyFeatures =
		VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	if ((swapchainProperties.optimalTilingFeatures & copyFeatures) != copyFeatures ||
		(rgbaProperties.optimalTilingFeatures & copyFeatures) != copyFeatures)
		return false;

	createInfo.imageUsage |= transferUsage;
	uint32_t desiredImages = createInfo.minImageCount + 2;
	if (capabilities.maxImageCount)
		desiredImages = std::min(desiredImages, capabilities.maxImageCount);
	createInfo.minImageCount = std::max(createInfo.minImageCount, desiredImages);
	createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	return true;
}

void SwitchLSFG_AttachSwapchain(bool mainWindow, VkSwapchainKHR swapchain, VkExtent2D extent,
	const VkImage* images, uint32_t imageCount, bool compatible)
{
	if (!mainWindow)
		return;
	ResetSwapchain();
	s_swapchain = swapchain;
	s_extent = extent;
	s_swapchainCompatible = compatible && imageCount >= 3;
	if (images && imageCount)
		s_swapchainImages.assign(images, images + imageCount);
	s_available.store(s_swapchainCompatible, std::memory_order_release);
}

void SwitchLSFG_DetachSwapchain(VkSwapchainKHR swapchain)
{
	if (swapchain && swapchain == s_swapchain)
		ResetSwapchain();
}

bool SwitchLSFG_Present(VkQueue queue, const VkPresentInfoKHR& presentInfo, VkResult& result)
{
	if (s_resetSessionRequested.exchange(false, std::memory_order_acq_rel))
	{
		DestroyRuntime();
		s_initAttempted = false;
		ResetTiming();
	}

	if (!s_swapchainCompatible || presentInfo.swapchainCount != 1 || !presentInfo.pSwapchains ||
		presentInfo.pSwapchains[0] != s_swapchain)
		return false;

	const int sessionOverride = s_sessionOverride.load(std::memory_order_acquire);
	const bool enabled = g_Config.bSwitchFrameGeneration && sessionOverride != 0;
	const bool inGame = GetUIState() == UISTATE_INGAME;
	const uint32_t speedControlState = s_speedControlState.load(std::memory_order_acquire);
	const bool fastForward = (speedControlState & 1) != 0;
	if (speedControlState != s_observedSpeedControlState) {
		ResetTiming();
		if (s_runtime) {
			lsfg_nx_reset_history(s_runtime);
		}
		s_observedSpeedControlState = speedControlState;
	}
	const bool requested = enabled && inGame && !fastForward;
	const float flowScale = g_Config.fSwitchFrameGenerationFlowScale == 0.5f ? 0.5f : 0.25f;
	if (flowScale != s_flowScale || g_Config.bSwitchFrameGenerationPerformance != s_performanceMode) {
		DestroyRuntime();
		s_initAttempted = false;
		s_flowScale = flowScale;
		s_performanceMode = g_Config.bSwitchFrameGenerationPerformance;
	}
	s_enabledRequested.store(enabled, std::memory_order_release);
	uint64_t sourceInterval = 0;
	if (!requested || s_rateDecision != 0)
		sourceInterval = ObserveSourcePresent();

	if (requested != s_previousRequested)
	{
		if (requested)
			s_rateDecision = s_sourceSamples >= 8 ? (SourceIsHighRate() ? 1 : 0) : -1;
		else
			ResetTiming();
		s_highRateSlowSamples = 0;
		s_previousRequested = requested;
	}

	if (!requested)
	{
		if (!enabled || !inGame) {
			DestroyRuntime();
			s_initAttempted = false;
		}
		s_highRatePassthrough.store(false, std::memory_order_release);
		return false;
	}

	if (s_rateDecision < 0 && s_sourceSamples >= 8)
		s_rateDecision = SourceIsHighRate() ? 1 : 0;
	if (s_rateDecision == 1)
	{
		s_highRatePassthrough.store(true, std::memory_order_release);
		if (sourceInterval >= 31'500'000)
			++s_highRateSlowSamples;
		else if (sourceInterval)
			s_highRateSlowSamples = 0;
		if (s_highRateSlowSamples < 16)
			return false;
		s_rateDecision = 0;
		s_highRateSlowSamples = 0;
	}
	if (s_rateDecision < 0)
		return false;

	s_highRatePassthrough.store(false, std::memory_order_release);
	if (!TryCreateRuntime(queue))
	{
		s_available.store(false, std::memory_order_release);
		s_enabledRequested.store(false, std::memory_order_release);
		return false;
	}
	if (lsfg_nx_present(s_runtime, queue, &presentInfo, &result))
		return true;

	s_available.store(false, std::memory_order_release);
	s_enabledRequested.store(false, std::memory_order_release);
	return false;
}

bool SwitchLSFG_IsPrepared()
{
	return s_prepared.load(std::memory_order_acquire);
}

SwitchLSFGDekoState SwitchLSFG_PollDeko() {
	SwitchLSFGDekoState state;
	if (!s_deko3d.load(std::memory_order_acquire)) {
		return state;
	}
	state.enabled = s_prepared.load(std::memory_order_acquire) && g_Config.bSwitchFrameGeneration &&
					s_sessionOverride.load(std::memory_order_acquire) != 0;
	state.speedControlState = s_speedControlState.load(std::memory_order_acquire);
	state.active = state.enabled && GetUIState() == UISTATE_INGAME && !(state.speedControlState & 1);
	state.flowScale = g_Config.fSwitchFrameGenerationFlowScale == 0.5f ? 0.5f : 0.25f;
	state.performance = g_Config.bSwitchFrameGenerationPerformance;
	state.resetSession = s_resetSessionRequested.exchange(false, std::memory_order_acq_rel);
	s_enabledRequested.store(state.enabled, std::memory_order_release);
	return state;
}

void SwitchLSFG_SetDekoStatus(bool available, bool highRate) {
	if (!s_deko3d.load(std::memory_order_acquire)) {
		return;
	}
	s_available.store(available && s_prepared.load(std::memory_order_acquire), std::memory_order_release);
	s_highRatePassthrough.store(available && highRate, std::memory_order_release);
	if (!available) {
		s_enabledRequested.store(false, std::memory_order_release);
	}
}

bool SwitchLSFG_IsInstalled()
{
	return s_installed.load(std::memory_order_acquire);
}

bool SwitchLSFG_IsAvailable()
{
	return s_available.load(std::memory_order_acquire);
}

bool SwitchLSFG_IsEnabled()
{
	return s_enabledRequested.load(std::memory_order_acquire);
}

bool SwitchLSFG_IsHighRatePassthrough()
{
	return s_enabledRequested.load(std::memory_order_acquire) &&
		s_highRatePassthrough.load(std::memory_order_acquire);
}

bool SwitchLSFG_RequestEnabled(bool enabled, float sourceFPS)
{
	if (enabled && (!s_available.load(std::memory_order_acquire) || sourceFPS < 27.0f || sourceFPS > 33.5f))
		return false;
	s_sessionOverride.store(enabled ? 1 : 0, std::memory_order_release);
	s_enabledRequested.store(enabled, std::memory_order_release);
	return true;
}

void SwitchLSFG_ResetSession()
{
	s_sessionOverride.store(-1, std::memory_order_release);
	s_enabledRequested.store(false, std::memory_order_release);
	s_resetSessionRequested.store(true, std::memory_order_release);
}
