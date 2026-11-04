// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project.
// Native resource binding and barriers adapted from Cemu-NX's DKFrameGenerator.

#include "Common/GPU/Deko3D/LSFGFrameGenerator.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

#include <lsfg/backend/binding.hpp>
#include <lsfg/backend/cache_load.hpp>
#include <lsfg/backend/layout.hpp>
#include <lsfg/backend/schedule.hpp>
#include <lsfg/common/dksh.hpp>

namespace Deko3DLSFG {
namespace {
namespace graph = lsfg::graph;
namespace backend = lsfg::backend;

constexpr uint64_t IMAGE_BUDGET = 128 * 1024 * 1024;
constexpr uint32_t DESCRIPTOR_SIZE = sizeof(DkImageDescriptor);
static_assert(sizeof(DkImageDescriptor) == sizeof(DkSamplerDescriptor));

DkImageFormat ImageFormat(graph::Format format) {
	switch (format) {
	case graph::Format::rgba8:
		return DkImageFormat_RGBA8_Unorm;
	case graph::Format::r8:
		return DkImageFormat_R8_Unorm;
	case graph::Format::rgba16f:
		return DkImageFormat_RGBA16_Float;
	}
	return DkImageFormat_None;
}

class Generator final : public FrameGenerator {
public:
	Generator(Services services, const Config &config) : services_(std::move(services)) {
		config_.performance = config.performance;
		config_.generated_frames = 1;
		config_.flow_numerator = 1;
		config_.flow_denominator = config.flowDenominator;
	}
	~Generator() override {
		for (auto *block : {&code_, &imagesMemory_, &descriptorsMemory_, &uniformsMemory_}) {
			services_.retire(std::move(*block));
		}
	}
	bool Initialize(const std::string &directory, uint32_t width, uint32_t height);
	bool Generate(DkCmdBuf commands, const DkImage &source, uint32_t width, uint32_t height) override;
	const DkImage *GeneratedImage() const override {
		return images_.size() > graph::history_image_count ? &images_[graph::history_image_count] : nullptr;
	}
	void Reset() override { framesSinceReset_ = 0; }
	bool Failed() const override { return failed_; }

private:
	bool Fail(const std::string &message) {
		failed_ = true;
		services_.log("Native LSFG: " + message);
		return false;
	}
	bool LoadModules();
	bool AllocateImages();
	void WriteDescriptors();
	bool Record(DkCmdBuf commands, uint32_t dispatch);
	void Barrier(DkCmdBuf commands, uint32_t invalidate);
	void Mark(uint32_t image, uint8_t hazard) {
		if (!hazards_[image]) {
			hazardImages_.push_back(image);
		}
		hazards_[image] |= hazard;
	}
	Services services_;
	graph::Config config_;
	lsfg::cache::Loaded cache_;
	backend::Plan plan_;
	backend::DescriptorLayout descriptors_;
	backend::Schedule schedule_;
	dk::UniqueMemBlock code_, imagesMemory_, descriptorsMemory_, uniformsMemory_;
	std::vector<DkShader> shaders_;
	std::vector<dk::Image> images_;
	std::vector<uint8_t> hazards_;
	std::vector<uint32_t> hazardImages_;
	uint32_t phase_ = 0, framesSinceReset_ = 0;
	bool failed_ = false;
};

bool Generator::Initialize(const std::string &directory, uint32_t width, uint32_t height) {
	if (!width || !height || !lsfg::succeeded(lsfg::cache::read(directory, cache_)) ||
		!lsfg::succeeded(graph::build(config_, cache_.graph))) {
		return Fail("shader cache is missing or invalid");
	}
	lsfg::cache::describe(cache_.header, cache_.graph);
	std::vector<lsfg::cache::PassEntry> passes;
	std::vector<lsfg::cache::SlotEntry> slots;
	for (const auto &pass : cache_.passes) {
		auto entry = pass.entry;
		entry.slot_first = (uint32_t)slots.size();
		passes.push_back(entry);
		slots.insert(slots.end(), pass.slots.begin(), pass.slots.end());
	}
	if (!lsfg::succeeded(lsfg::cache::validate(cache_.header, passes, slots, cache_.graph)) ||
		!lsfg::succeeded(backend::schedule(cache_.graph, schedule_)) || !lsfg::succeeded(backend::describe(cache_.graph, descriptors_))) {
		return Fail("shader bindings do not match the frame-generation graph");
	}
	backend::Request request;
	request.config = config_;
	request.precision = lsfg::cache::Precision::high;
	request.output = {width, height};
	request.memory_budget_bytes = IMAGE_BUDGET;
	backend::Rejection why;
	if (!backend::accept(cache_, request, plan_, why)) {
		return Fail("cannot run at " + std::to_string(width) + "x" + std::to_string(height) + ": " + std::string(why.reason));
	}
	if (plan_.max_scratch_bytes_per_warp > services_.scratchPerWarp) {
		return Fail("shader scratch requirements exceed the queue allocation");
	}
	for (uint32_t dispatch = 0; dispatch < cache_.graph.dispatches.size(); ++dispatch) {
		for (uint32_t phase = 0; phase < cache_.graph.dispatches[dispatch].variant_count; ++phase) {
			backend::DispatchBinding binding;
			if (!lsfg::succeeded(backend::bind(cache_, plan_, descriptors_, dispatch, phase, binding))) {
				return Fail("a compute dispatch cannot be bound");
			}
		}
	}
	if (!LoadModules() || !AllocateImages()) {
		return false;
	}
	descriptorsMemory_ = services_.allocate((descriptors_.image_descriptors + backend::sampler_descriptor_count) * DESCRIPTOR_SIZE,
											DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "LSFG descriptors");
	if (plan_.uniform_buffers) {
		uniformsMemory_ = services_.allocate(plan_.uniform_buffers * backend::uniform_buffer_stride,
											 DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "LSFG constants");
	}
	if (!descriptorsMemory_ || (plan_.uniform_buffers && !uniformsMemory_)) {
		return Fail("could not allocate descriptors or constants");
	}
	WriteDescriptors();
	if (uniformsMemory_) {
		auto *bytes = (uint8_t *)uniformsMemory_.getCpuAddr();
		for (uint32_t i = 0; i < plan_.uniform_buffers; ++i) {
			const auto constants = graph::constant_buffer(i, config_);
			memcpy(bytes + i * backend::uniform_buffer_stride, &constants, sizeof(constants));
		}
	}
	hazards_.assign(images_.size(), 0);
	hazardImages_.reserve(images_.size());
	services_.log("Native LSFG ready: " + std::to_string(width) + "x" + std::to_string(height) + ", flow=1/" +
				  std::to_string(config_.flow_denominator) + (config_.performance ? ", performance" : ", quality"));
	return true;
}

bool Generator::LoadModules() {
	backend::Arena arena;
	std::vector<uint32_t> offsets;
	for (const auto &pass : cache_.passes) {
		lsfg::dksh::FileHeader header;
		memcpy(&header, pass.dksh.data(), sizeof(header));
		offsets.push_back(arena.place(header.code_size, DK_SHADER_CODE_ALIGNMENT));
	}
	(void)arena.place(DK_SHADER_CODE_UNUSABLE_SIZE, 1);
	if (arena.overflowed()) {
		return Fail("shader code does not fit in one memory block");
	}
	code_ = services_.allocate(arena.block_size(), DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
							   "LSFG shader code");
	if (!code_) {
		return Fail("could not allocate shader code");
	}
	auto *code = (uint8_t *)code_.getCpuAddr();
	shaders_.resize(cache_.passes.size());
	for (size_t i = 0; i < shaders_.size(); ++i) {
		const auto &pass = cache_.passes[i];
		lsfg::dksh::FileHeader header;
		memcpy(&header, pass.dksh.data(), sizeof(header));
		memcpy(code + offsets[i], pass.dksh.data() + header.control_size, header.code_size);
		DkShaderMaker maker;
		dkShaderMakerDefaults(&maker, code_, offsets[i]);
		maker.control = pass.dksh.data();
		dkShaderInitialize(&shaders_[i], &maker);
		if (!dkShaderIsValid(&shaders_[i])) {
			return Fail("a compute shader could not be initialized");
		}
	}
	return true;
}

bool Generator::AllocateImages() {
	backend::Arena arena;
	std::vector<dk::ImageLayout> layouts(plan_.images.size());
	std::vector<uint32_t> offsets;
	for (size_t i = 0; i < layouts.size(); ++i) {
		const auto &image = plan_.images[i];
		uint32_t flags = image.role == graph::ImageRole::history ? 0 : DkImageFlags_UsageLoadStore;
		if (image.role == graph::ImageRole::history || image.role == graph::ImageRole::generated) {
			flags |= DkImageFlags_Usage2DEngine;
		}
		dk::ImageLayoutMaker{services_.device}
			.setFlags(flags)
			.setFormat(ImageFormat(image.format))
			.setDimensions(image.extent.width, image.extent.height)
			.initialize(layouts[i]);
		offsets.push_back(arena.place(layouts[i].getSize(), layouts[i].getAlignment()));
	}
	if (arena.overflowed() || arena.block_size() > IMAGE_BUDGET) {
		return Fail("frame images exceed the 128 MiB budget");
	}
	imagesMemory_ = services_.allocate(arena.block_size(), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image | DkMemBlockFlags_ZeroFillInit,
									   "LSFG image pool");
	if (!imagesMemory_) {
		return Fail("could not allocate frame images");
	}
	images_.resize(layouts.size());
	for (size_t i = 0; i < images_.size(); ++i) {
		images_[i].initialize(layouts[i], imagesMemory_, offsets[i]);
	}
	return true;
}

void Generator::WriteDescriptors() {
	auto *table = (uint8_t *)descriptorsMemory_.getCpuAddr();
	for (size_t i = 0; i < images_.size(); ++i) {
		const auto &entry = descriptors_.images[i];
		DkImageView view;
		dkImageViewDefaults(&view, &images_[i]);
		if (entry.sampled != backend::no_descriptor) {
			dkImageDescriptorInitialize((DkImageDescriptor *)(table + entry.sampled * DESCRIPTOR_SIZE), &view, false, false);
		}
		if (entry.storage != backend::no_descriptor) {
			dkImageDescriptorInitialize((DkImageDescriptor *)(table + entry.storage * DESCRIPTOR_SIZE), &view, true, false);
		}
	}
	auto *samplers = (DkSamplerDescriptor *)(table + descriptors_.image_descriptors * DESCRIPTOR_SIZE);
	for (uint32_t i = 0; i < backend::sampler_descriptor_count; ++i) {
		DkSampler sampler;
		dkSamplerDefaults(&sampler);
		sampler.minFilter = sampler.magFilter = DkFilter_Linear;
		sampler.mipFilter = DkMipFilter_Linear;
		for (auto &wrap : sampler.wrapMode) {
			wrap =
				i == (uint32_t)graph::Sampler::edge || i == backend::introduced_sampler ? DkWrapMode_ClampToEdge : DkWrapMode_ClampToBorder;
		}
		if (i == (uint32_t)graph::Sampler::border_white) {
			for (auto &channel : sampler.borderColor) {
				channel.value_f = 1;
			}
		}
		dkSamplerDescriptorInitialize(&samplers[i], &sampler);
	}
}

void Generator::Barrier(DkCmdBuf commands, uint32_t invalidate) {
	dkCmdBufBarrier(commands, DkBarrier_Primitives, invalidate);
	for (uint32_t image : hazardImages_) {
		hazards_[image] = 0;
	}
	hazardImages_.clear();
}

bool Generator::Record(DkCmdBuf commands, uint32_t dispatch) {
	backend::DispatchBinding binding;
	if (!lsfg::succeeded(backend::bind(cache_, plan_, descriptors_, dispatch, phase_, binding))) {
		return Fail("a compute dispatch binding became invalid");
	}
	bool wait = false;
	uint32_t invalidate = 0;
	for (uint32_t i = 0; i < binding.texture_count; ++i) {
		if (hazards_[binding.textures[i].image] & 2) {
			wait = true;
			invalidate |= DkInvalidateFlags_Image;
		}
	}
	for (uint32_t i = 0; i < binding.storage_count; ++i) {
		wait |= hazards_[binding.storages[i].image] != 0;
	}
	if (wait) {
		Barrier(commands, invalidate);
	}
	const DkShader *shader = &shaders_[binding.pass];
	dkCmdBufBindShaders(commands, DkStageFlag_Compute, &shader, 1);
	std::array<DkResHandle, backend::max_texture_slots> textures{};
	uint32_t textureSlots = 0;
	for (uint32_t i = 0; i < binding.texture_count; ++i) {
		const auto &texture = binding.textures[i];
		Mark(texture.image, 1);
		textures[texture.slot] = dkMakeTextureHandle(texture.descriptor, texture.sampler);
		textureSlots = std::max(textureSlots, texture.slot + 1);
	}
	if (textureSlots) {
		dkCmdBufBindTextures(commands, DkStage_Compute, 0, textures.data(), textureSlots);
	}
	std::array<DkResHandle, backend::max_storage_slots> storages{};
	uint32_t storageSlots = 0;
	for (uint32_t i = 0; i < binding.storage_count; ++i) {
		const auto &storage = binding.storages[i];
		Mark(storage.image, 2);
		storages[storage.slot] = dkMakeImageHandle(storage.descriptor);
		storageSlots = std::max(storageSlots, storage.slot + 1);
	}
	if (storageSlots) {
		dkCmdBufBindImages(commands, DkStage_Compute, 0, storages.data(), storageSlots);
	}
	if (binding.uniform_slot != backend::no_slot) {
		dkCmdBufBindUniformBuffer(commands, DkStage_Compute, binding.uniform_slot,
								  uniformsMemory_.getGpuAddr() + binding.uniform_buffer * backend::uniform_buffer_stride,
								  backend::uniform_buffer_stride);
	}
	dkCmdBufDispatchCompute(commands, binding.groups_x, binding.groups_y, 1);
	return true;
}

bool Generator::Generate(DkCmdBuf commands, const DkImage &source, uint32_t width, uint32_t height) {
	if (failed_) {
		return false;
	}
	if (width != plan_.output.width || height != plan_.output.height) {
		return Fail("output size changed without rebuilding the image pool");
	}
	DkImageView sourceView, historyView;
	dkImageViewDefaults(&sourceView, &source);
	dkImageViewDefaults(&historyView, &images_[phase_ % graph::history_image_count]);
	const DkImageRect rect{0, 0, 0, width, height, 1};
	dkCmdBufBarrier(commands, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Shader);
	dkCmdBufBlitImage(commands, &sourceView, &rect, &historyView, &rect, DkBlitFlag_FilterNearest | DkBlitFlag_ModeBlit, 0);
	dkCmdBufBindImageDescriptorSet(commands, descriptorsMemory_.getGpuAddr(), descriptors_.image_descriptors);
	dkCmdBufBindSamplerDescriptorSet(commands, descriptorsMemory_.getGpuAddr() + descriptors_.image_descriptors * DESCRIPTOR_SIZE,
									 backend::sampler_descriptor_count);
	dkCmdBufBarrier(commands, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
	for (uint32_t image : hazardImages_) {
		hazards_[image] = 0;
	}
	hazardImages_.clear();
	bool recorded = true;
	for (uint32_t dispatch = 0; recorded && dispatch < cache_.graph.dispatches.size(); ++dispatch) {
		recorded = Record(commands, dispatch);
	}
	dkCmdBufBarrier(commands, DkBarrier_Primitives, DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);
	if (!recorded) {
		return false;
	}
	phase_ = (phase_ + 1) % schedule_.cycle;
	return ++framesSinceReset_ > schedule_.warmup_frames;
}
} // namespace

std::unique_ptr<FrameGenerator> CreateFrameGenerator(Services services, const Config &config, const std::string &directory, uint32_t width,
													 uint32_t height) {
	auto generator = std::make_unique<Generator>(std::move(services), config);
	return generator->Initialize(directory, width, height) ? std::move(generator) : nullptr;
}

} // namespace Deko3DLSFG
