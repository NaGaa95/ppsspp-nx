// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

// Production method bodies are inserted by test-switch-lsfg-speed.py.
#define VK_USE_64_BIT_PTR_DEFINES 0
#include "Common/GPU/Vulkan/SwitchLSFG.h"
#include "Common/GPU/Vulkan/lsfg/lsfg_bridge.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

constexpr auto queueHandle = (uintptr_t)1;
uint64_t now = 1;
unsigned creates = 0, destroys = 0, generatedPresents = 0, originalPresents = 0, captures = 0;
unsigned dispatches = 0, extraAcquires = 0, fenceWaits = 0;
unsigned lastCaptured = 0;
bool installed = true;
enum { UISTATE_MENU, UISTATE_INGAME } ui = UISTATE_INGAME;
int GetUIState() { return ui; }
bool FileReadable(const char *) { return installed; }
bool EnableNvkNoCbuf() { return true; }
uint64_t MonotonicNs() { return now; }
struct {
	bool bSwitchFrameGeneration = true, bSwitchFrameGenerationPerformance = true;
	float fSwitchFrameGenerationFlowScale = .25f;
	bool bVSync = true, bLowLatencyPresent = false;
} g_Config;
// GLOBALS

namespace vk {
struct Vulkan;
struct Fence {
	bool wait(const Vulkan &) {
		++fenceWaits;
		return true;
	}
	void reset(const Vulkan &) {}
	VkFence handle() const { return 4; }
};
struct Semaphore {
	VkSemaphore value;
	VkSemaphore handle() const { return value; }
};
struct CommandBuffer {
	void begin(const Vulkan &) const {}
	void end(const Vulkan &) const {}
	void submit(const Vulkan &, std::vector<VkSemaphore> waits, VkSemaphore, uint64_t, std::vector<VkSemaphore> signals, VkSemaphore,
				uint64_t, VkFence = VK_NULL_HANDLE) const {
		assert(waits.size() == 1 && signals.size() == 1);
	}
};
VkResult AcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t *image) {
	*image = extraAcquires++ % 4;
	return VK_SUCCESS;
}
VkResult QueuePresentKHR(VkQueue, const VkPresentInfoKHR *info) {
	assert(info->waitSemaphoreCount == 1 && info->swapchainCount == 1 && info->pImageIndices[0] < 4);
	if (info->pWaitSemaphores[0] == 2) {
		++generatedPresents;
	} else {
		assert(info->pWaitSemaphores[0] == 3);
		++originalPresents;
	}
	return VK_SUCCESS;
}
struct Vulkan {
	struct Functions {
		decltype(&vk::AcquireNextImageKHR) AcquireNextImageKHR = vk::AcquireNextImageKHR;
		decltype(&vk::QueuePresentKHR) QueuePresentKHR = vk::QueuePresentKHR;
	} functions;
	VkDevice dev() const { return reinterpret_cast<VkDevice>(1); }
	VkQueue queue() const { return reinterpret_cast<VkQueue>(queueHandle); }
	const Functions &df() const { return functions; }
};
} // namespace vk
struct FrameSlot {
	vk::CommandBuffer frame, restore;
	vk::Fence completion;
	vk::Semaphore acquired{1}, generated{2}, original{3};
	bool pending = false;
};
struct FakeContext {
	unsigned phase = 0;
};
struct Backend {
	VkImage sourceImage(FakeContext &, size_t index) {
		assert(index < 2);
		return 10 + index;
	}
	VkImage destinationImage(FakeContext &, size_t index) {
		assert(index == 0);
		return 20;
	}
	void recordFrame(FakeContext &context, const vk::CommandBuffer &) {
		assert(lastCaptured == context.phase % 2);
		++context.phase;
		++dispatches;
	}
};
struct LsfgNxRuntime {
	vk::Vulkan gpu;
	const vk::Vulkan *vulkan = &gpu;
	VkSwapchainKHR swapchain = 100;
	VkExtent2D extent{1280, 720};
	std::vector<VkImage> swapchainImages{1000, 1001, 1002, 1003};
	std::vector<FrameSlot> slots = std::vector<FrameSlot>(4);
	std::unique_ptr<Backend> backend = std::make_unique<Backend>();
	FakeContext local;
	FakeContext *context = &local;
	// BRIDGE_FIELDS
	// BRIDGE_METHODS
	void recordCapture(const vk::CommandBuffer &, VkImage source, VkImage target, bool) {
		assert(source >= 1000 && source <= 1003 && target >= 10 && target <= 11);
		lastCaptured = target - 10;
		++captures;
	}
	void recordOutput(const vk::CommandBuffer &, VkImage output, VkImage target) {
		assert(output == 20 && target >= 1000 && target <= 1003);
	}
	void recordOriginal(const vk::CommandBuffer &, VkImage source, VkImage target) {
		assert(source >= 10 && source <= 11 && target >= 1000 && target <= 1003);
	}
};
extern "C" LsfgNxRuntime *lsfg_nx_create(const LsfgNxCreateInfo *) {
	++creates;
	return new LsfgNxRuntime;
}
extern "C" void lsfg_nx_destroy(LsfgNxRuntime *runtime) {
	if (runtime) {
		++destroys;
		delete runtime;
	}
}
extern "C" bool lsfg_nx_present(LsfgNxRuntime *runtime, VkQueue queue, const VkPresentInfoKHR *info, VkResult *result) {
	*result = runtime->present(queue, *info);
	return true;
}
// RESET_EXPORT
bool TryCreateRuntime(VkQueue) {
	if (!s_runtime) {
		s_runtime = lsfg_nx_create(nullptr);
	}
	return true;
}
// CONTROLS

namespace Draw {
enum class PresentMode { FIFO = 1, IMMEDIATE = 2, MAILBOX = 4 };
bool operator&(PresentMode a, PresentMode b) { return ((int)a & (int)b) != 0; }
PresentMode &operator|=(PresentMode &a, PresentMode b) { return a = (PresentMode)((int)a | (int)b); }
struct DeviceCaps {
	PresentMode presentModesSupported;
	bool presentInstantModeChange;
	int presentMaxInterval = 1;
	bool multipleSwapchains = false;
};
struct DrawContext {
	DeviceCaps caps{PresentMode::FIFO, false};
	const DeviceCaps &GetDeviceCaps() { return caps; }
};
} // namespace Draw
#define PPSSPP_PLATFORM(name) 1
#define _dbg_assert_ assert
struct FrameTiming {
	bool fastForwardSkipFlip_ = false;
	Draw::PresentMode presentMode_{};
	void ComputePresentMode(Draw::DrawContext *draw, bool fastForward);
} timing;
// TIMING

struct CapsVulkan {
	void *GetPresentation() { return nullptr; }
	std::vector<VkPresentModeKHR> GetAvailablePresentModes() {
		return {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR};
	}
};
Draw::DeviceCaps SwitchCaps() {
	using Draw::PresentMode;
	CapsVulkan instance;
	auto *vulkan = &instance;
	Draw::DeviceCaps caps_{};
	// PRESENT_CAPS
	return caps_;
}
#undef PPSSPP_PLATFORM
#define PPSSPP_PLATFORM(name) 0
Draw::DeviceCaps DesktopCaps() {
	using Draw::PresentMode;
	CapsVulkan instance;
	auto *vulkan = &instance;
	Draw::DeviceCaps caps_{};
	// PRESENT_CAPS
	return caps_;
}

void Present(unsigned count, uint64_t interval = 33'333'333, bool mustBypass = false) {
	const uint32_t index = 0;
	const VkSemaphore wait = 44;
	VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	info.swapchainCount = 1;
	info.pSwapchains = &s_swapchain;
	info.pImageIndices = &index;
	info.waitSemaphoreCount = 1;
	info.pWaitSemaphores = &wait;
	for (unsigned i = 0; i < count; ++i) {
		now += interval;
		VkResult result = VK_NOT_READY;
		bool handled = SwitchLSFG_Present(reinterpret_cast<VkQueue>(queueHandle), info, result);
		if (mustBypass) {
			assert(!handled && result == VK_NOT_READY);
			assert(info.waitSemaphoreCount == 1 && info.pWaitSemaphores == &wait && info.pImageIndices == &index);
		}
	}
}
int main() {
	SwitchLSFG_Configure(true, .25f, true, false);
	s_swapchain = 100;
	s_swapchainCompatible = true;
	s_available = true;
	Draw::DrawContext draw;
	draw.caps = SwitchCaps();
	assert(draw.caps.presentModesSupported == Draw::PresentMode::FIFO && !draw.caps.presentInstantModeChange);
	assert(DesktopCaps().presentModesSupported == (Draw::PresentMode)7);
	timing.ComputePresentMode(&draw, false);
	Present(30);
	assert(s_runtime && generatedPresents > 0 && creates == 1 && destroys == 0);
	for (unsigned burst = 0; burst < 8; ++burst) {
		const auto generated = generatedPresents, acquired = extraAcquires, recorded = dispatches, waited = fenceWaits;
		auto *const retained = s_runtime;
		g_Config.bLowLatencyPresent = burst % 2;
		timing.ComputePresentMode(&draw, true);
		assert(timing.fastForwardSkipFlip_ && timing.presentMode_ == Draw::PresentMode::FIFO);
		Present(15, 16'666'667, true);
		assert(generatedPresents == generated && extraAcquires == acquired && dispatches == recorded && fenceWaits == waited);
		assert(s_runtime == retained && creates == 1 && destroys == 0 && SwitchLSFG_IsEnabled() && g_Config.bSwitchFrameGeneration);
		timing.ComputePresentMode(&draw, false);
		Present(8, 33'333'333, true);
		const auto originals = originalPresents;
		Present(3);
		assert(originalPresents == originals + 3 && generatedPresents == generated && s_runtime == retained);
		Present(20);
		assert(generatedPresents > generated && creates == 1 && destroys == 0 && !SwitchLSFG_IsHighRatePassthrough());
	}
	// Transitions between render polls must still invalidate history.
	const auto generated = generatedPresents;
	SwitchLSFG_SetFastForward(true);
	SwitchLSFG_SetFastForward(false);
	Present(8, 33'333'333, true);
	Present(3);
	assert(generatedPresents == generated);
	Present(20);
	assert(generatedPresents > generated);
	// A configured/session-disabled LSFG must remain disabled after fast-forward.
	assert(SwitchLSFG_RequestEnabled(false, 60));
	SwitchLSFG_SetFastForward(true);
	Present(10, 16'666'667, true);
	SwitchLSFG_SetFastForward(false);
	Present(20, 33'333'333, true);
	assert(!s_runtime && destroys == 1 && !SwitchLSFG_IsEnabled());
	assert(SwitchLSFG_RequestEnabled(true, 30));
	Present(30);
	assert(s_runtime && creates == 2);
	ui = UISTATE_MENU;
	Present(2, 33'333'333, true);
	assert(!s_runtime && destroys == 2);
	// Deko3D's instant present mode switches still reach the same publisher.
	draw.caps = {(Draw::PresentMode)3, true};
	g_Config.bVSync = true;
	g_Config.bLowLatencyPresent = false;
	timing.ComputePresentMode(&draw, true);
	assert(timing.presentMode_ == Draw::PresentMode::IMMEDIATE && !timing.fastForwardSkipFlip_ && (s_speedControlState & 1));
	timing.ComputePresentMode(&draw, false);
	assert(timing.presentMode_ == Draw::PresentMode::FIFO && !(s_speedControlState & 1));
	g_Config.bVSync = false;
	timing.ComputePresentMode(&draw, false);
	assert(timing.presentMode_ == Draw::PresentMode::IMMEDIATE && !(s_speedControlState & 1));
	timing.ComputePresentMode(nullptr, true);
	assert(s_speedControlState & 1);
	DestroyRuntime();
	puts("LSFG speed checks passed: production timing publication, FIFO skip-flip, immediate mode, Vulkan bypass, "
		 "retained runtime, repeated resume, fresh history with preserved dispatch phase, semaphore ownership, short bursts, session/pause "
		 "behavior.");
}
