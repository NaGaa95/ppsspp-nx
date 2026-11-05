#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 or later.

"""Exercise Deko3D's actual retirement/resize methods with a bounded fake GPU.

No Switch SDK is needed. Run on Linux with a C++17 compiler and sanitizers.
--source-ref can reproduce the regression against a previous Git revision.
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def body(source, marker):
    start = source.index("{", source.index(marker))
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", help="Read the backend from this Git revision")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    path = "Common/GPU/Deko3D/DekoContext.cpp"
    source = subprocess.check_output(["git", "show", f"{args.source_ref}:{path}"], cwd=root, text=True) if args.source_ref else (root / path).read_text()
    methods = []
    for name in ("CollectRetired", "Wait", "ResetRenderBindings", "StopThreads", "StartThreads", "Resize"):
        marker = f"void Context::{name}("
        if marker in source:
            implementation = body(source, marker)
        elif name == "Wait":
            implementation = body(source, "void Wait() override")
        else:
            # thin3d's default StopThreads/StartThreads are no-ops.
            implementation = "{}"
        methods.append(f"void Context::{name}() {implementation}")
    retire = body(source, "void Retire(dk::UniqueMemBlock block)")
    fixture = r'''
#include "Common/GPU/thin3d.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
using namespace Draw;
Draw::RefCountedObject::~RefCountedObject() = default;
bool Draw::RefCountedObject::Release() {
	if (--refcount_ == 0) {
		delete this;
		return true;
	}
	return false;
}
struct Gpu {
	size_t bytes = 0, allocations = 0;
	size_t budget = 64 * 1024 * 1024;
	bool idle = true, error = false;
};
struct Block {
	Gpu *gpu;
	size_t size;
	Block(Gpu *g, size_t bytes) : gpu(g), size(bytes) {
		assert(gpu->bytes + bytes <= gpu->budget);
		gpu->bytes += bytes;
		++gpu->allocations;
	}
	~Block() {
		assert(gpu->idle);
		gpu->bytes -= size;
	}
};
namespace dk {
using UniqueMemBlock = std::unique_ptr<Block>;
}
struct Queue {
	Gpu *gpu;
	explicit operator bool() const { return true; }
	bool isInErrorState() const { return gpu->error; }
	void waitIdle() {
		assert(!gpu->error);
		gpu->idle = true;
	}
};
struct Device {
	Gpu gpu;
	Queue queue{&gpu};
	std::mutex mutex;
	std::vector<dk::UniqueMemBlock> pending;
	void Retire(dk::UniqueMemBlock block);
	dk::UniqueMemBlock Allocate(size_t size) { return std::make_unique<Block>(&gpu, size); }
};
struct Image : Texture {
	std::shared_ptr<Device> owner;
	dk::UniqueMemBlock memory;
	Image(std::shared_ptr<Device> d, int w, int h) : owner(d), memory(d->Allocate((size_t)w * h * 4)) {
		width_ = w;
		height_ = h;
	}
	~Image() override { owner->Retire(std::move(memory)); }
};
struct FramebufferDeko : Framebuffer {
	AutoRef<Image> color, depth;
	FramebufferDeko(std::shared_ptr<Device> d, int w, int h) {
		color.reset(new Image(d, w, h));
		depth.reset(new Image(d, w, h));
	}
};
struct Frame {
	bool submitted = false;
	std::vector<dk::UniqueMemBlock> retired;
};
struct NWindow {
	uint32_t width = 1280, height = 720;
};
int nwindowGetDimensions(NWindow *window, uint32_t *w, uint32_t *h) {
	*w = window->width;
	*h = window->height;
	return 0;
}
#define R_SUCCEEDED(x) ((x) == 0)
#undef ERROR_LOG
#define ERROR_LOG(...) assert(false)
struct Context {
	struct { void Invalidate() {} } resourceBindings_;
	std::shared_ptr<Device> device_ = std::make_shared<Device>();
	std::array<Frame, 3> frames_;
	Framebuffer *target_ = nullptr;
	AutoRef<FramebufferDeko> targetRef_;
	std::array<Image *, 3> textures_{};
	std::array<AutoRef<Image>, 3> textureRefs_;
	bool targetBound_ = false, stateDirty_ = false, resourcesStopped_ = false;
	std::unique_ptr<int> swapchain_;
	std::array<AutoRef<Image>, 3> backbuffers_;
	AutoRef<Image> backDepth_;
	AutoRef<Image> generatedDisplay_;
	struct Generator { void Reset() {} };
	std::unique_ptr<Generator> frameGenerator_;
	struct Pacer { void Reset() {} } framePacer_;
	bool frameGenerationFailed_ = false;
	NWindow window;
	void *window_ = &window;
	int acquired_ = -1, submits = 0, swapchains = 0;
	void CollectRetired();
	void Wait();
	void ResetRenderBindings();
	void StopThreads();
	void StartThreads();
	void Resize();
	void Submit(bool wait) {
		++submits;
		if (wait && !device_->queue.isInErrorState()) {
			device_->queue.waitIdle();
		}
	}
	bool ReportQueueFault(const char *) { return device_->queue.isInErrorState(); }
	bool Swapchain(std::string *) {
		++swapchains;
		swapchain_ = std::make_unique<int>(1);
		for (auto &image : backbuffers_) {
			image.reset(new Image(device_, window.width, window.height));
		}
		backDepth_.reset(new Image(device_, window.width, window.height));
		return true;
	}
	void Bind(FramebufferDeko *fb) {
		target_ = fb;
		targetBound_ = true;
		targetRef_ = fb;
		textures_[0] = fb->color.ptr;
		textureRefs_[0] = fb->color.ptr;
	}
};
'''
    fixture += f"\nvoid Device::Retire(dk::UniqueMemBlock block) {retire}\n" + "\n".join(methods)
    fixture += r'''
void TestWait() {
	Context c;
	for (auto &frame : c.frames_) {
		frame.retired.push_back(c.device_->Allocate(1024));
		frame.submitted = true;
	}
	c.device_->Retire(c.device_->Allocate(2048));
	c.device_->gpu.idle = false;
	c.Wait();
	assert(c.device_->gpu.bytes == 0);
	for (auto &frame : c.frames_) {
		assert(!frame.submitted && frame.retired.empty());
	}
	assert(c.device_->pending.empty());
}
void TestRenderResolution() {
	Context c;
	std::vector<AutoRef<FramebufferDeko>> buffers(3);
	for (int iteration = 0; iteration < 100; ++iteration) {
		const int scale = 1 + iteration % 4;
		c.StopThreads();
		for (auto &fb : buffers) {
			fb.reset(nullptr);
		}
		c.StartThreads();
		assert(!c.targetBound_ && !c.target_ && !c.targetRef_);
		assert(!c.textures_[0] && !c.textureRefs_[0]);
		assert(c.device_->gpu.bytes == 0);
		for (auto &fb : buffers) {
			fb.reset(new FramebufferDeko(c.device_, 480 * scale, 272 * scale));
		}
		c.Bind(buffers.front().ptr);
		c.device_->gpu.idle = false;
	}
	c.StopThreads();
	for (auto &fb : buffers) {
		fb.reset(nullptr);
	}
	c.StartThreads();
	assert(c.device_->gpu.bytes == 0);
}
void TestExternalOwner() {
	Context c;
	AutoRef<FramebufferDeko> fb;
	fb.reset(new FramebufferDeko(c.device_, 480, 272));
	AutoRef<Image> cache(fb->color.ptr);
	c.Bind(fb.ptr);
	c.device_->gpu.idle = false;
	c.StopThreads();
	fb.reset(nullptr);
	c.StartThreads();
	assert(c.device_->gpu.bytes == 480 * 272 * 4);
	assert(cache->Width() == 480);
	cache.reset(nullptr);
	c.Wait();
	assert(c.device_->gpu.bytes == 0);
}
void TestDisplayResize() {
	Context c;
	std::string error;
	c.Swapchain(&error);
	const size_t allocations = c.device_->gpu.allocations;
	c.Resize();
	assert(c.swapchains == 1 && c.submits == 0 && c.device_->gpu.allocations == allocations);
	for (int i = 0; i < 100; ++i) {
		c.textureRefs_[0] = c.backbuffers_[0].ptr;
		c.textures_[0] = c.backbuffers_[0].ptr;
		c.device_->gpu.idle = false;
		c.window.width = i % 2 ? 1280 : 1920;
		c.window.height = i % 2 ? 720 : 1080;
		c.Resize();
		assert(c.device_->gpu.bytes == (size_t)c.window.width * c.window.height * 4 * 4);
		assert(c.backbuffers_[0]->Width() == (int)c.window.width && c.acquired_ == -1);
	}
	c.StopThreads();
	c.swapchain_ = nullptr;
	c.backDepth_.reset(nullptr);
	for (auto &image : c.backbuffers_) {
		image.reset(nullptr);
	}
	c.StartThreads();
	assert(c.device_->gpu.bytes == 0);
}
void TestFaultedQueue() {
	Context c;
	c.frames_[0].retired.push_back(c.device_->Allocate(1024));
	c.device_->Retire(c.device_->Allocate(2048));
	c.device_->gpu.idle = false;
	c.device_->gpu.error = true;
	c.StopThreads();
	c.StartThreads();
	assert(c.device_->gpu.bytes == 3072);
	c.device_->gpu.error = false;
	c.Wait();
	assert(c.device_->gpu.bytes == 0);
}
int main() {
	TestWait();
	TestRenderResolution();
	TestExternalOwner();
	TestDisplayResize();
	TestFaultedQueue();
	puts("Deko3D lifetime tests passed: idle reclamation, 100 render resizes, external owners, 100 display resizes, "
		 "unchanged-size no-op, faulted-queue retention.");
}
'''
    with tempfile.TemporaryDirectory(prefix="ppsspp-deko-lifetimes-") as directory:
        folder = Path(directory)
        test = folder / "test.cpp"
        test.write_text(fixture)
        executable = folder / "test"
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        subprocess.run(compiler + ["-std=c++17", "-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(root), str(test), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
