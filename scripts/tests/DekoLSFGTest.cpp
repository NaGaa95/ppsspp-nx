// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

// Host-side SDK test double. The actual native frame generator is linked below.
#include "Common/GPU/Deko3D/LSFGFrameGenerator.h"
#include "Common/GPU/Deko3D/LSFGFramePacer.h"
#include "Common/GPU/Deko3D/LSFGShaders.h"
#include <lsfg/common/cache_store.hpp>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

namespace dk::detail {
struct MemBlock {
	void *bytes;
	uint32_t size, flags;
};
} // namespace dk::detail
namespace {
bool gpuBusy = false, shaderDirty = false;
size_t liveBytes = 0, peakBytes = 0;
unsigned allocations = 0, failAllocation = 0, imageID = 0, shaderID = 0, dispatches = 0;
std::vector<dk::UniqueMemBlock> retired;
struct Layout {
	uint64_t size;
	uint32_t width, height, format, flags;
};
struct Image {
	Layout layout;
	DkMemBlock block;
	uint32_t offset, id;
};
struct Descriptor {
	const DkImage *image;
	bool storage;
};
static_assert(sizeof(Layout) <= sizeof(DkImageLayout) && sizeof(Image) <= sizeof(DkImage));
static_assert(sizeof(Descriptor) <= sizeof(DkImageDescriptor));
template <class T, class Object> T Read(const Object *object) {
	T result;
	memcpy(&result, object, sizeof(result));
	return result;
}
template <class T, class Object> void Write(Object *object, const T &value) {
	memset(object, 0, sizeof(*object));
	memcpy(object, &value, sizeof(value));
}
struct Commands {
	DkGpuAddr images = 0;
	uint32_t imageCount = 0, samplerCount = 0;
	std::vector<uint32_t> textures, storages;
	std::set<uint32_t> reads, writes, textureDirty;
} command;
uint32_t DescriptorImage(uint32_t handle, bool storage) {
	const uint32_t index = handle & 0xFFFFF;
	assert(index < command.imageCount);
	const auto desc = Read<Descriptor>((const DkImageDescriptor *)command.images + index);
	assert(desc.storage == storage && desc.image);
	const auto image = Read<Image>(desc.image);
	assert(image.block && image.offset + image.layout.size <= image.block->size);
	return image.id;
}
void Drain() {
	gpuBusy = false;
	retired.clear();
	assert(liveBytes == 0);
}
Deko3DLSFG::Services Services() {
	Deko3DLSFG::Services result;
	result.device = reinterpret_cast<DkDevice>(1);
	result.scratchPerWarp = 32 * 1024;
	result.allocate = [](uint32_t size, uint32_t flags, const char *) {
		if (++allocations == failAllocation) {
			return dk::UniqueMemBlock{};
		}
		size = (size + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);
		assert(size && size < 128 * 1024 * 1024);
		auto *block = new dk::detail::MemBlock{std::aligned_alloc(DK_MEMBLOCK_ALIGNMENT, size), size, flags};
		assert(block->bytes);
		memset(block->bytes, 0, size);
		liveBytes += size;
		peakBytes = std::max(peakBytes, liveBytes);
		dk::MemBlock handle{block};
		return dk::UniqueMemBlock{std::move(handle)};
	};
	result.retire = [](dk::UniqueMemBlock block) {
		if (block) {
			retired.push_back(std::move(block));
		}
	};
	result.log = [](const std::string &) {};
	return result;
}
void TestPacing() {
	Deko3DLSFG::FramePacer pacing;
	uint64_t now = 1;
	for (int i = 0; i < 40; ++i) {
		auto result = pacing.Observe(now += 33'333'333, true, true);
		if (i >= 8) {
			assert(result.generate && !result.highRate);
		}
	}
	assert(pacing.Observe(now += 1'000'000'000, true, true).reset);
	assert(!pacing.Observe(now += 33'333'333, false, true).generate);
	pacing.Reset();
	for (int i = 0; i < 40; ++i) {
		auto result = pacing.Observe(now += 16'666'667, true, true);
		assert(!result.generate);
		if (i >= 8) {
			assert(result.highRate);
		}
	}
	for (int i = 0; i < 16; ++i) {
		auto result = pacing.Observe(now += 33'333'333, true, true);
		assert(result.generate == (i == 15));
	}
	pacing.Reset();
	for (int i = 0; i < 20; ++i) {
		assert(!pacing.Observe(now += 33'333'333, true, false).generate);
	}
	assert(pacing.Observe(now += 33'333'333, true, true).generate);
}
} // namespace

extern "C" {
void dkMemBlockDestroy(DkMemBlock block) {
	assert(!gpuBusy);
	liveBytes -= block->size;
	std::free(block->bytes);
	delete block;
}
void *dkMemBlockGetCpuAddr(DkMemBlock block) { return block->bytes; }
DkGpuAddr dkMemBlockGetGpuAddr(DkMemBlock block) { return (DkGpuAddr)block->bytes; }
uint32_t dkMemBlockGetSize(DkMemBlock block) { return block->size; }
void dkImageLayoutInitialize(DkImageLayout *object, const DkImageLayoutMaker *maker) {
	const uint32_t bpp = maker->format == DkImageFormat_RGBA16_Float ? 8 : maker->format == DkImageFormat_R8_Unorm ? 1 : 4;
	const uint64_t size = ((uint64_t)maker->dimensions[0] * bpp + 63) / 64 * 64 * maker->dimensions[1];
	Write(object, Layout{(size + 4095) & ~4095ULL, maker->dimensions[0], maker->dimensions[1], (uint32_t)maker->format, maker->flags});
}
uint64_t dkImageLayoutGetSize(const DkImageLayout *object) { return Read<Layout>(object).size; }
uint32_t dkImageLayoutGetAlignment(const DkImageLayout *) { return 4096; }
void dkImageInitialize(DkImage *object, const DkImageLayout *layout, DkMemBlock block, uint32_t offset) {
	const auto value = Read<Layout>(layout);
	assert(offset + value.size <= block->size && offset % 4096 == 0);
	Write(object, Image{value, block, offset, ++imageID});
}
void dkShaderInitialize(DkShader *object, const DkShaderMaker *maker) {
	assert(maker->control && maker->codeMem && maker->codeOffset % DK_SHADER_CODE_ALIGNMENT == 0);
	assert(maker->codeOffset < maker->codeMem->size);
	Write(object, ++shaderID);
	shaderDirty = true;
}
bool dkShaderIsValid(const DkShader *shader) { return Read<uint32_t>(shader) != 0; }
void dkImageDescriptorInitialize(DkImageDescriptor *object, const DkImageView *view, bool storage, bool decay) {
	assert(!decay && view->pImage);
	const auto image = Read<Image>(view->pImage);
	if (storage) {
		assert(image.layout.flags & DkImageFlags_UsageLoadStore);
	}
	Write(object, Descriptor{view->pImage, storage});
}
void dkSamplerDescriptorInitialize(DkSamplerDescriptor *object, const DkSampler *sampler) {
	assert(sampler->minFilter == DkFilter_Linear && sampler->magFilter == DkFilter_Linear);
	memset(object, 0, sizeof(*object));
}
void dkCmdBufBarrier(DkCmdBuf, DkBarrier mode, uint32_t flags) {
	if (flags & DkInvalidateFlags_Shader) {
		shaderDirty = false;
	}
	if (mode != DkBarrier_None) {
		command.reads.clear();
		command.writes.clear();
	}
	if (flags & DkInvalidateFlags_Image) {
		command.textureDirty.clear();
	}
}
void dkCmdBufBlitImage(DkCmdBuf, const DkImageView *source, const DkImageRect *srcRect, const DkImageView *destination,
					   const DkImageRect *dstRect, uint32_t, uint32_t) {
	const auto src = Read<Image>(source->pImage), dst = Read<Image>(destination->pImage);
	assert(srcRect->width == dstRect->width && srcRect->height == dstRect->height);
	assert(dstRect->width == dst.layout.width && dstRect->height == dst.layout.height);
	assert(src.layout.format == DkImageFormat_RGBA8_Unorm && dst.layout.format == DkImageFormat_RGBA8_Unorm);
	assert(command.writes.empty() && command.reads.empty());
	command.writes.insert(dst.id);
	command.textureDirty.insert(dst.id);
}
void dkCmdBufBindImageDescriptorSet(DkCmdBuf, DkGpuAddr address, uint32_t count) {
	command.images = address;
	command.imageCount = count;
}
void dkCmdBufBindSamplerDescriptorSet(DkCmdBuf, DkGpuAddr, uint32_t count) { command.samplerCount = count; }
void dkCmdBufBindShaders(DkCmdBuf, uint32_t stages, const DkShader *const shaders[], uint32_t count) {
	assert(stages == DkStageFlag_Compute && count == 1 && dkShaderIsValid(shaders[0]));
	command.textures.clear();
	command.storages.clear();
}
void dkCmdBufBindTextures(DkCmdBuf, DkStage stage, uint32_t first, const DkResHandle handles[], uint32_t count) {
	assert(stage == DkStage_Compute && first == 0 && count <= 32);
	for (uint32_t i = 0; i < count; ++i) {
		assert((handles[i] >> 20) < command.samplerCount);
		command.textures.push_back(DescriptorImage(handles[i], false));
	}
}
void dkCmdBufBindImages(DkCmdBuf, DkStage stage, uint32_t first, const DkResHandle handles[], uint32_t count) {
	assert(stage == DkStage_Compute && first == 0 && count <= 8);
	for (uint32_t i = 0; i < count; ++i) {
		command.storages.push_back(DescriptorImage(handles[i], true));
	}
}
void dkCmdBufBindUniformBuffers(DkCmdBuf, DkStage stage, uint32_t first, const DkBufExtents buffers[], uint32_t count) {
	assert(stage == DkStage_Compute && first == 0 && count == 1 && buffers[0].size == 256 && buffers[0].addr % 256 == 0);
}
void dkCmdBufDispatchCompute(DkCmdBuf, uint32_t x, uint32_t y, uint32_t z) {
	assert(!shaderDirty);
	assert(x && y && z == 1);
	for (uint32_t image : command.textures) {
		assert(!command.writes.count(image));
		assert(!command.textureDirty.count(image));
	}
	for (uint32_t image : command.storages) {
		assert(!command.reads.count(image) && !command.writes.count(image));
	}
	for (uint32_t image : command.textures) {
		command.reads.insert(image);
	}
	for (uint32_t image : command.storages) {
		command.writes.insert(image);
		command.textureDirty.insert(image);
	}
	++dispatches;
}
} // extern C

int main(int argc, char **argv) {
	assert(argc == 3);
	TestPacing();
	std::atomic<bool> cancel{false};
	for (bool performance : {true, false}) {
		const auto prepared = Deko3DLSFG::PrepareShaders(performance, cancel, argv[1], argv[2]);
		if (!prepared.error.empty()) {
			fprintf(stderr, "%s\n", prepared.error.c_str());
			return 1;
		}
		lsfg::cache::Loaded cache;
		assert(lsfg::succeeded(lsfg::cache::read(prepared.directory, cache)) && cache.passes.size() == 25);
		const auto manifest = std::filesystem::path(prepared.directory) / "manifest.bin";
		const auto modified = std::filesystem::last_write_time(manifest);
		assert(Deko3DLSFG::PrepareShaders(performance, cancel, argv[1], argv[2]).error.empty());
		assert(std::filesystem::last_write_time(manifest) == modified);
		for (unsigned flow : {4U, 2U}) {
			for (const auto size : {std::pair{1280u, 720u}, {1920u, 1080u}}) {
				allocations = failAllocation = dispatches = 0;
				command = {};
				auto generator =
					Deko3DLSFG::CreateFrameGenerator(Services(), {performance, flow}, prepared.directory, size.first, size.second);
				assert(generator && !generator->Failed());
				const auto initialAllocations = allocations;
				assert(initialAllocations <= 4 && liveBytes < 128 * 1024 * 1024);
				DkImage source;
				Write(&source, Image{{0, size.first, size.second, DkImageFormat_RGBA8_Unorm, DkImageFlags_Usage2DEngine}, nullptr, 0, 0});
				gpuBusy = true;
				unsigned generated = 0;
				for (int frame = 0; frame < 16; ++frame) {
					generated += generator->Generate(reinterpret_cast<DkCmdBuf>(1), source, size.first, size.second);
				}
				assert(generated > 0 && dispatches > 0 && allocations == initialAllocations);
				assert(generator->GeneratedImage());
				const auto output = Read<Image>(generator->GeneratedImage());
				assert(output.layout.width == size.first && output.layout.height == size.second);
				generator->Reset();
				assert(!generator->Generate(reinterpret_cast<DkCmdBuf>(1), source, size.first, size.second));
				assert(allocations == initialAllocations);
				assert(!generator->Generate(reinterpret_cast<DkCmdBuf>(1), source, size.first + 1, size.second) && generator->Failed());
				generator.reset();
				assert(retired.size() == initialAllocations && liveBytes > 0);
				Drain();
				printf("Native LSFG runtime passed: %s, flow=1/%u, %ux%u, dispatches=%u, allocations=%u\n",
					   performance ? "performance" : "quality", flow, size.first, size.second, dispatches, initialAllocations);
			}
		}
		for (unsigned failure = 1; failure <= 4; ++failure) {
			allocations = 0;
			failAllocation = failure;
			auto generator = Deko3DLSFG::CreateFrameGenerator(Services(), {performance, 4}, prepared.directory, 1280, 720);
			assert((generator == nullptr) == (allocations >= failure));
			generator.reset();
			Drain();
		}
		allocations = failAllocation = 0;
		assert(!Deko3DLSFG::CreateFrameGenerator(Services(), {performance, 2}, prepared.directory, 8192, 8192));
		Drain();
	}
	cancel = true;
	assert(Deko3DLSFG::PrepareShaders(true, cancel, argv[1], argv[2]).cancelled);
	printf("Native LSFG checks passed: real DLL shaders, warm caches, pacing, descriptors, image hazards, "
		   "no per-frame allocation, retirement, allocation failures, memory budget, cancellation. Peak fake GPU bytes=%zu\n",
		   peakBytes);
}
