// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

// Methods are extracted from production code by test-deko3d-lsfg-context.py.
#include "Common/GPU/Deko3D/LSFGFramePacer.h"
#include "Common/GPU/Deko3D/LSFGShaders.h"
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

bool installed = true, available = false, highRate = false;
unsigned nvkChanges = 0, preparations = 0, generations = 0, resets = 0, waits = 0, allocations = 0, liveResources = 0;
std::atomic_bool s_installed{false}, s_prepared{false}, s_available{false}, s_enabledRequested{false};
std::atomic_bool s_highRatePassthrough{false}, s_resetSessionRequested{false}, s_deko3d{false};
std::atomic_int s_sessionOverride{-1};
std::atomic_uint32_t s_speedControlState{0};
float s_flowScale = .25f;
bool s_performanceMode = true;
constexpr auto kShaderDllPath = "Lossless.dll";
enum { UISTATE_MENU, UISTATE_INGAME } ui = UISTATE_INGAME;
int GetUIState() { return ui; }
bool FileReadable(const char *) { return installed; }
bool EnableNvkNoCbuf() {
	++nvkChanges;
	return true;
}
struct {
	bool bSwitchFrameGeneration = true;
	float fSwitchFrameGenerationFlowScale = .25f;
	bool bSwitchFrameGenerationPerformance = true;
} g_Config;
struct SwitchLSFGDekoState {
	bool enabled = false, active = false, resetSession = false;
	uint32_t speedControlState = 0;
	float flowScale = .25f;
	bool performance = true;
};
// CONTROL_IMPLEMENTATIONS

enum { DkResult_Success, DkResult_Fail, DkBarrier_Full, DkInvalidateFlags_Image, DkBlitFlag_FilterNearest };
struct DkImage {
	unsigned id = 0;
};
struct DkImageView {
	const DkImage *image;
};
struct DkImageRect {
	unsigned x, y, z, width, height, depth;
};
void dkImageViewDefaults(DkImageView *view, const DkImage *image) { view->image = image; }
bool gpuBusy = false, queueFault = false;
std::vector<std::unique_ptr<int>> retired;
std::vector<unsigned> presented;
unsigned lastImage = 0, submissions = 0, acquiredSlot = 0, actualPresents = 0;
bool outstandingAcquire = false;
struct Swapchain {
	int interval = -1;
	void setSwapInterval(int value) { interval = value; }
};
struct Queue {
	int acquireImage(const Swapchain &) {
		assert(!outstandingAcquire && !queueFault);
		outstandingAcquire = true;
		return acquiredSlot++ % 3;
	}
	void presentImage(const Swapchain &, int index) {
		assert(outstandingAcquire && !queueFault && index >= 0 && index < 3 && submissions);
		outstandingAcquire = false;
		presented.push_back(lastImage);
	}
	void flush() {}
};
namespace dk {
using UniqueMemBlock = std::unique_ptr<int>;
struct CmdBuf {
	operator void *() const { return nullptr; }
	void barrier(int, int) {}
	void blitImage(const DkImageView &source, const DkImageRect &, const DkImageView &, const DkImageRect &, int) {
		lastImage = source.image->id;
	}
};
} // namespace dk
struct Device {
	int device = 1;
	Queue queue;
	dk::UniqueMemBlock Allocate(unsigned, unsigned, const char *) {
		++allocations;
		++liveResources;
		return std::make_unique<int>(1);
	}
	void Retire(dk::UniqueMemBlock block) {
		if (block) {
			retired.push_back(std::move(block));
		}
	}
};
std::shared_ptr<Device> device = std::make_shared<Device>();
struct Image {
	DkImage image;
	int width = 1280, height = 720;
	dk::UniqueMemBlock memory;
	Image(unsigned id) : image{id}, memory(device->Allocate(1, 0, "image")) {}
	~Image() { device->Retire(std::move(memory)); }
	int Width() const { return width; }
	int Height() const { return height; }
};
template <class T> struct Ref {
	T *ptr = nullptr;
	~Ref() { reset(nullptr); }
	T *operator->() { return ptr; }
	explicit operator bool() const { return ptr != nullptr; }
	void reset(T *value) {
		delete ptr;
		ptr = value;
	}
};
namespace Deko3DLSFG {
struct Config {
	bool performance = true;
	unsigned flowDenominator = 4;
};
struct Services {
	int device;
	unsigned scratchPerWarp;
	std::function<dk::UniqueMemBlock(unsigned, unsigned, const char *)> allocate;
	std::function<void(dk::UniqueMemBlock)> retire;
	std::function<void(const std::string &)> log;
};
bool failRuntime = false, failInitialization = false, failPreparation = false;
struct FrameGenerator {
	DkImage output{900};
	unsigned frames = 0;
	dk::UniqueMemBlock memory = device->Allocate(1, 0, "generator");
	~FrameGenerator() { device->Retire(std::move(memory)); }
	void Reset() {
		++resets;
		frames = 0;
	}
	bool Generate(void *, const DkImage &, unsigned, unsigned) {
		++generations;
		return ++frames > 2 && !failRuntime;
	}
	bool Failed() const { return failRuntime; }
	const DkImage *GeneratedImage() const { return &output; }
};
std::unique_ptr<FrameGenerator> CreateFrameGenerator(Services, Config, const std::string &directory, unsigned width, unsigned height) {
	assert(!directory.empty() && width == 1280 && height == 720);
	return failInitialization ? nullptr : std::make_unique<FrameGenerator>();
}
PreparedShaders PrepareShaders(bool performance, const std::atomic<bool> &cancel, const std::string &, const std::string &) {
	std::this_thread::sleep_for(std::chrono::milliseconds(2));
	PreparedShaders result;
	result.cancelled = cancel;
	result.directory = performance ? "performance" : "quality";
	if (failPreparation) {
		result.error = "injected preparation failure";
	}
	return result;
}
} // namespace Deko3DLSFG
void SetCurrentThreadName(const char *) {}
enum class ThreadAffinityRole { BACKGROUND };
void SetCurrentThreadAffinity(ThreadAffinityRole) {}
#undef INFO_LOG
#undef ERROR_LOG
#define INFO_LOG(...) ((void)0)
#define ERROR_LOG(...) ((void)0)
template <class... T> std::string StringFromFormat(const char *, T...) { return {}; }
void saltynx_frame(int) { ++actualPresents; }
enum class PresentMode { FIFO, IMMEDIATE };
enum class TextureType { LINEAR2D };
enum class DataFormat { R8G8B8A8_UNORM };
struct TextureDesc {
	TextureType type;
	int width, height, depth, mipLevels;
	DataFormat format;
};
namespace InvalidationCallbackFlags {
constexpr int COMMAND_BUFFER_STATE = 1, RENDER_PASS_STATE = 2;
}
constexpr unsigned SCRATCH_PER_WARP = 32 * 1024, FRAME_COUNT = 3;
struct Context {
	struct { void Invalidate() {} } resourceBindings_;
	std::shared_ptr<Device> device_ = device;
	std::array<Ref<Image>, 3> backbuffers_;
	Ref<Image> generatedDisplay_;
	Image window{50};
	Image *primarySwapchain_ = &window;
	std::unique_ptr<Deko3DLSFG::FrameGenerator> frameGenerator_;
	Deko3DLSFG::FramePacer framePacer_;
	Deko3DLSFG::Config frameGenerationConfig_;
	SwitchLSFGDekoState frameGenerationState_;
	std::future<Deko3DLSFG::PreparedShaders> framePreparation_;
	std::thread framePreparationThread_;
	std::shared_ptr<std::atomic<bool>> framePreparationCancel_;
	std::string frameShaderDirectory_;
	bool framePreparationPerformance_ = true, framePreparationStarted_ = false, frameGenerationFailed_ = false;
	bool active_ = false, displayUsed_ = true, stateDirty_ = false, targetBound_ = false;
	int frameGenerationDecision_ = -1, acquired_ = -1;
	unsigned frameIndex_ = 0, frameCount_ = 0;
	uint64_t nowNs_ = 1;
	PresentMode presentMode_{};
	Swapchain swapchain_;
	std::function<void(int)> invalidated_ = [](int flags) { assert(flags == 3); };
	Context() {
		for (auto &image : backbuffers_) {
			image.reset(new Image(100));
		}
	}
	~Context() {
		if (framePreparationCancel_) {
			*framePreparationCancel_ = true;
		}
		if (framePreparationThread_.joinable()) {
			framePreparationThread_.join();
		}
		Wait();
		frameGenerator_.reset();
		generatedDisplay_.reset(nullptr);
		for (auto &image : backbuffers_) {
			image.reset(nullptr);
		}
		CollectRetired();
	}
	dk::CmdBuf Commands() { return {}; }
	void Submit(bool wait) {
		++submissions;
		gpuBusy = !wait;
	}
	void Wait() {
		++waits;
		if (!queueFault) {
			gpuBusy = false;
		}
	}
	void CollectRetired() {
		assert(!gpuBusy);
		liveResources -= retired.size();
		retired.clear();
	}
	void ResetRenderBindings() {
		stateDirty_ = true;
		targetBound_ = false;
	}
	bool ReportQueueFault(const char *) { return queueFault; }
	Image *CreateImage(TextureDesc desc, bool render, bool zeroFill) {
		assert(render && zeroFill && desc.width == 1280 && desc.height == 720);
		return new Image(800);
	}
	void UpdateFrameGeneration();
	bool PresentImage(const DkImage &, uint32_t, uint32_t);
	void Present(PresentMode);
	void Frame(uint64_t interval = 33'333'333, PresentMode mode = PresentMode::FIFO) {
		UpdateFrameGeneration();
		active_ = displayUsed_ = true;
		nowNs_ += interval;
		if (!generatedDisplay_) {
			acquired_ = device_->queue.acquireImage(swapchain_);
			lastImage = 100;
		}
		Present(mode);
	}
	void Ready() {
		for (int i = 0; i < 100 && !frameGenerator_ && !frameGenerationFailed_; ++i) {
			UpdateFrameGeneration();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		assert(frameGenerator_ || frameGenerationFailed_);
	}
};
// CONTEXT_IMPLEMENTATIONS

int main() {
	SwitchLSFG_Configure(true, .25f, true, true);
	assert(nvkChanges == 0 && SwitchLSFG_IsPrepared() && SwitchLSFG_IsAvailable() && SwitchLSFG_IsInstalled());
	auto state = SwitchLSFG_PollDeko();
	assert(state.enabled && state.active && state.performance && state.flowScale == .25f);
	ui = UISTATE_MENU;
	assert(!SwitchLSFG_PollDeko().active && SwitchLSFG_PollDeko().enabled);
	ui = UISTATE_INGAME;
	assert(!SwitchLSFG_RequestEnabled(true, 60) && SwitchLSFG_RequestEnabled(false, 60));
	assert(!SwitchLSFG_PollDeko().enabled);
	assert(SwitchLSFG_RequestEnabled(true, 30) && SwitchLSFG_PollDeko().enabled);
	SwitchLSFG_ResetSession();
	assert(SwitchLSFG_PollDeko().resetSession && !SwitchLSFG_PollDeko().resetSession);
	SwitchLSFG_SetDekoStatus(true, true);
	assert(SwitchLSFG_IsHighRatePassthrough());
	SwitchLSFG_Configure(true, .25f, true, false);
	assert(nvkChanges == 1 && !SwitchLSFG_IsAvailable() && !SwitchLSFG_PollDeko().enabled);
	installed = false;
	SwitchLSFG_Configure(true, .25f, true, true);
	assert(!SwitchLSFG_IsPrepared() && !SwitchLSFG_IsInstalled() && !SwitchLSFG_IsAvailable());
	installed = true;
	SwitchLSFG_Configure(true, .25f, true, true);
	{
		Context c;
		c.UpdateFrameGeneration();
		assert(c.framePreparation_.valid() && !c.frameGenerator_);
		c.Ready();
		assert(c.generatedDisplay_ && c.frameShaderDirectory_ == "performance");
		const auto initialAllocations = allocations;
		for (int i = 0; i < 40; ++i) {
			c.Frame();
		}
		assert(allocations == initialAllocations && generations && presented.size() > c.frameCount_);
		assert(!outstandingAcquire && presented.back() == 800 && actualPresents == presented.size());
		for (size_t i = 0; i < presented.size(); ++i) {
			if (presented[i] == 900) {
				assert(i + 1 < presented.size() && presented[i + 1] == 800);
			}
		}
		const auto generatedBeforeSpeed = generations, waitsBeforeSpeed = waits;
		auto *const retainedGenerator = c.frameGenerator_.get();
		auto speedPresents = presented.size();
		SwitchLSFG_SetFastForward(true);
		for (int i = 0; i < 20; ++i) {
			c.Frame(10'000'000, PresentMode::IMMEDIATE);
		}
		assert(generations == generatedBeforeSpeed && presented.size() == speedPresents + 20 && c.swapchain_.interval == 0);
		assert(allocations == initialAllocations && waits == waitsBeforeSpeed && c.frameGenerator_.get() == retainedGenerator);
		assert(SwitchLSFG_IsEnabled() && SwitchLSFG_IsAvailable() && g_Config.bSwitchFrameGeneration);
		SwitchLSFG_SetFastForward(false);
		speedPresents = presented.size();
		c.Frame();
		assert(presented.size() == speedPresents + 1 && !SwitchLSFG_IsHighRatePassthrough());
		for (int i = 0; i < 20; ++i) {
			c.Frame();
		}
		assert(generations > generatedBeforeSpeed && allocations == initialAllocations && waits == waitsBeforeSpeed);
		const auto resetsBeforeBurst = resets;
		SwitchLSFG_SetFastForward(true);
		SwitchLSFG_SetFastForward(false);
		speedPresents = presented.size();
		c.Frame();
		assert(resets > resetsBeforeBurst && presented.size() == speedPresents + 1);
		ui = UISTATE_MENU;
		auto count = presented.size();
		c.Frame();
		assert(presented.size() == count + 1 && presented.back() == 800);
		ui = UISTATE_INGAME;
		g_Config.fSwitchFrameGenerationFlowScale = .5f;
		c.Frame();
		assert(c.frameGenerationConfig_.flowDenominator == 2 && allocations == initialAllocations + 2);
		g_Config.bSwitchFrameGenerationPerformance = false;
		c.UpdateFrameGeneration();
		c.Ready();
		assert(c.frameShaderDirectory_ == "quality" && !c.frameGenerationConfig_.performance);
		// A faulted queue must keep its pools alive until queue teardown.
		gpuBusy = queueFault = true;
		g_Config.bSwitchFrameGeneration = false;
		const auto live = liveResources;
		c.UpdateFrameGeneration();
		assert(c.frameGenerator_ && c.generatedDisplay_ && liveResources == live);
		queueFault = false;
		c.UpdateFrameGeneration();
		assert(!c.frameGenerator_ && !c.generatedDisplay_ && !gpuBusy);
		count = presented.size();
		c.Frame();
		assert(presented.size() == count + 1 && presented.back() == 100);
		g_Config.bSwitchFrameGeneration = true;
		c.Ready();
		Deko3DLSFG::failRuntime = true;
		for (int i = 0; i < 12; ++i) {
			c.Frame();
		}
		assert(c.frameGenerationFailed_ && !c.frameGenerator_ && !c.generatedDisplay_);
		Deko3DLSFG::failRuntime = false;
	}
	// Rapid quality changes cancel the old worker and discard its result.
	g_Config = {true, .25f, true};
	{
		Context c;
		c.UpdateFrameGeneration();
		g_Config.bSwitchFrameGenerationPerformance = false;
		c.UpdateFrameGeneration();
		c.Ready();
		assert(c.frameShaderDirectory_ == "quality" && !c.framePreparationThread_.joinable());
	}
	Deko3DLSFG::failPreparation = true;
	{
		Context c;
		c.Ready();
		assert(c.frameGenerationFailed_ && !c.frameGenerator_ && !c.generatedDisplay_);
		c.Frame();
		assert(presented.back() == 100);
	}
	Deko3DLSFG::failPreparation = false;
	Deko3DLSFG::failInitialization = true;
	{
		Context c;
		c.Ready();
		assert(c.frameGenerationFailed_ && !c.frameGenerator_ && !c.generatedDisplay_);
	}
	Deko3DLSFG::failInitialization = false;
	g_Config = {true, .25f, true};
	{
		Context c;
		c.Ready();
		const auto generated = generations;
		for (int i = 0; i < 40; ++i) {
			c.Frame(16'666'667);
		}
		assert(SwitchLSFG_IsHighRatePassthrough() && generations == generated);
		c.UpdateFrameGeneration();
		assert(SwitchLSFG_IsHighRatePassthrough());
	}
	{
		Context c;
		c.UpdateFrameGeneration();
		assert(c.framePreparationThread_.joinable());
	}
	gpuBusy = false;
	liveResources -= retired.size();
	retired.clear();
	assert(liveResources == 0 && !outstandingAcquire);
	puts("Native LSFG controls/presentation passed: backend isolation, asynchronous warmup, two ordered presents, "
		 "persistent resources, fast-forward bypass/resume/bursts, pause, flow/quality changes, fault retention, "
		 "ordinary presentation on failure, cancellation.");
}
