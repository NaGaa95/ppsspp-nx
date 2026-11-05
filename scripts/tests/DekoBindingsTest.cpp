// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

#include <deko3d.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <random>

// TEXTURE_SLOT_COUNT
struct GPU {
	std::array<DkResHandle, MAX_TEXTURE_SLOTS> textures{};
	std::array<std::array<DkBufExtents, 8>, 2> uniforms{};
	unsigned textureBinds = 0, uniformBinds = 0;
	void Reset() {
		textures.fill(0xFFFFFFFF);
		for (auto &stage : uniforms) {
			stage.fill({0xDEAD, 0});
		}
	}
};

extern "C" void dkCmdBufBindTextures(DkCmdBuf command, DkStage stage, uint32_t first, const DkResHandle *handles, uint32_t count) {
	auto &gpu = *reinterpret_cast<GPU *>(command);
	assert(stage == DkStage_Fragment && first + count <= gpu.textures.size());
	for (unsigned i = 0; i < count; ++i) {
		gpu.textures[first + i] = handles[i];
	}
	++gpu.textureBinds;
}
extern "C" void dkCmdBufBindUniformBuffers(DkCmdBuf command, DkStage stage, uint32_t first, const DkBufExtents *buffers, uint32_t count) {
	auto &gpu = *reinterpret_cast<GPU *>(command);
	assert((stage == DkStage_Vertex || stage == DkStage_Fragment) && first + count <= 8);
	auto &bound = gpu.uniforms[stage == DkStage_Vertex ? 0 : 1];
	for (unsigned i = 0; i < count; ++i) {
		bound[first + i] = buffers[i];
	}
	++gpu.uniformBinds;
}

// BINDING_IMPLEMENTATIONS

int main() {
	GPU gpu;
	dk::CmdBuf cmd(reinterpret_cast<DkCmdBuf>(&gpu));
	ResourceBindings cache;
	std::array<DkResHandle, MAX_TEXTURE_SLOTS> handles{};
	std::array<Upload, 8> uploads{};
	auto check = [&] {
		assert(gpu.textures == handles);
		for (const auto &stage : gpu.uniforms) {
			for (unsigned i = 0; i < uploads.size(); ++i) {
				if (uploads[i].gpu.addr) {
					assert(stage[i].addr == uploads[i].gpu.addr && stage[i].size == uploads[i].gpu.size);
				}
			}
		}
	};
	// All-zero texture handles must still be emitted on the first draw.
	gpu.Reset();
	cache.Apply(cmd, handles, uploads);
	check();
	assert(gpu.textureBinds == 1 && gpu.uniformBinds == 0);
	for (unsigned i = 0; i < uploads.size(); ++i) {
		uploads[i].gpu = {0x1000 + i * 256, 256};
	}
	handles[0] = dkMakeTextureHandle(1, 2);
	handles.back() = dkMakeTextureHandle(3, 4);
	cache.Apply(cmd, handles, uploads);
	check();
	assert(gpu.textureBinds == 2 && gpu.uniformBinds == 16);
	for (unsigned i = 0; i < 10000; ++i) {
		cache.Apply(cmd, handles, uploads);
	}
	assert(gpu.textureBinds == 2 && gpu.uniformBinds == 16);
	// Equal addresses with a different extent still require a bind in both stages.
	uploads[3].gpu.size = 512;
	cache.Apply(cmd, handles, uploads);
	check();
	assert(gpu.textureBinds == 2 && gpu.uniformBinds == 18);
	uploads[3].gpu.addr += 0x10000;
	cache.Apply(cmd, handles, uploads);
	check();
	assert(gpu.uniformBinds == 20);
	// An inactive upload leaves the previous GPU binding untouched, as before.
	const Upload saved = uploads[2];
	uploads[2] = {};
	cache.Apply(cmd, handles, uploads);
	uploads[2] = saved;
	cache.Apply(cmd, handles, uploads);
	check();
	assert(gpu.uniformBinds == 20);
	// Sampler/view changes and unused texture slots must reach the GPU.
	handles.back() = dkMakeTextureHandle(3, 5);
	cache.Apply(cmd, handles, uploads);
	handles.fill(0);
	cache.Apply(cmd, handles, uploads);
	check();
	assert(gpu.textureBinds == 4 && gpu.uniformBinds == 20);
	// Fresh command streams, recycled descriptor indices and external rendering can
	// all reuse identical numeric handles/addresses after losing their GPU bindings.
	for (unsigned transition = 0; transition < 6; ++transition) {
		const auto textureBinds = gpu.textureBinds, uniformBinds = gpu.uniformBinds;
		gpu.Reset();
		cache.Invalidate();
		cache.Apply(cmd, handles, uploads);
		check();
		assert(gpu.textureBinds == textureBinds + 1 && gpu.uniformBinds == uniformBinds + 16);
	}
	// Changing shader/texture masks, upload addresses and frame boundaries must
	// preserve the same effective state as rebinding everything before every draw.
	std::minstd_rand random(12345);
	for (unsigned draw = 0; draw < 10000; ++draw) {
		const auto slot = random() % uploads.size();
		if (draw % 3 == 0) {
			uploads[slot].gpu = {0x1000 + (random() % 64) * 256, (uint32_t)(1 + random() % 4) * 256};
		}
		if (draw % 5 == 0) {
			handles[slot % handles.size()] = random() % 2 ? dkMakeTextureHandle(random() % 32, random() % 8) : 0;
		}
		if (draw % 101 == 0) {
			gpu.Reset();
			cache.Invalidate();
		}
		cache.Apply(cmd, handles, uploads);
		check();
	}
	puts("Deko bindings passed: repeated draws, buffer extents, texture/sampler changes, invalidation and 10000 mixed draws.");
}
