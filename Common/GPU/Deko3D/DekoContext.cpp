// Copyright (c) 2012- PPSSPP Project.
// Memory and shader setup follow Dolphin-NX's Deko3D backend (GPL-2.0-or-later).

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#include "Common/GPU/Deko3D/DekoContext.h"

#include <deko3d.hpp>
#include <switch.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <unordered_map>
#include <variant>
#include <vector>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "Common/File/FileUtil.h"
#include "Common/Crypto/sha1.h"
#include "Common/GPU/Deko3D/DekoMemoryPool.h"
#include "Common/GPU/Deko3D/ShaderCompiler.h"
#include "Common/GPU/Deko3D/LSFGFrameGenerator.h"
#include "Common/GPU/Deko3D/LSFGFramePacer.h"
#include "Common/GPU/Deko3D/LSFGShaders.h"
#include "Common/GPU/Vulkan/SwitchLSFG.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/Log.h"
#include "Common/System/Request.h"
#include "Common/StringUtils.h"
#include "Common/SwitchSaltyNX.h"
#include "Common/Thread/ThreadUtil.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/System.h"

namespace Draw {
namespace {

constexpr unsigned FRAME_COUNT = 3;
constexpr unsigned IMAGE_DESCRIPTORS = 65536;
constexpr unsigned SAMPLER_DESCRIPTORS = 1024;
constexpr uint32_t UPLOAD_CHUNK_SIZE = 8 * 1024 * 1024;
constexpr uint32_t COMMAND_CHUNK_SIZE = 1024 * 1024;
constexpr uint32_t SCRATCH_PER_WARP = 32 * 1024;
constexpr uint32_t TEXTURE_SLAB_SIZE = 2 * 1024 * 1024;
constexpr uint32_t SHADER_SLAB_SIZE = 1024 * 1024;

// deko3d's default callback exits the app on any error. Log it instead, and let the caller handle the failure.
void DekoDebug(void *, const char *context, DkResult result, const char *message) {
	if (result != DkResult_Success) {
		ERROR_LOG(Log::G3D, "deko3d %s: %s (%d)", context ? context : "", message ? message : "", (int)result);
	}
}

uint32_t Align(uint32_t value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

using MemoryPool = DekoMemoryPool<dk::UniqueMemBlock>;
using RetiredMemory = std::variant<dk::UniqueMemBlock, MemoryPool::Slice>;

struct Device {
	dk::UniqueDevice device;
	dk::UniqueQueue queue;
	MemoryPool texturePool, shaderPool;
	std::atomic<bool> shaderCodeDirty{false};
	std::mutex mutex;
	std::vector<RetiredMemory> pending;
	uint64_t nextImage = 1, nextSampler = 1;
	~Device() {
		if (queue && !queue.isInErrorState()) {
			queue.waitIdle();
		}
		pending.clear();
	}
	template <typename Memory>
	void Retire(Memory memory) {
		if (memory) {
			std::lock_guard<std::mutex> guard(mutex);
			pending.emplace_back(std::move(memory));
		}
	}
	dk::UniqueMemBlock Allocate(uint32_t size, uint32_t flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
								const char *tag = "GPU buffer") {
		const uint32_t bytes = Align(size, DK_MEMBLOCK_ALIGNMENT);
		dk::MemBlock block = dk::MemBlockMaker{device, bytes}.setFlags(flags).create();
		return dk::UniqueMemBlock{std::move(block)};
	}
};

struct Image : Texture {
	std::shared_ptr<Device> owner;
	dk::UniqueMemBlock memory;
	MemoryPool::Slice pooledMemory;
	dk::Image image;
	DkImageView view{};
	int levels = 1;
	uint64_t identity = 0;
	~Image() override {
		owner->Retire(std::move(memory));
		owner->Retire(std::move(pooledMemory));
	}
	void Dimensions(const TextureDesc &desc) {
		width_ = desc.width;
		height_ = desc.height;
		depth_ = desc.depth;
		format_ = desc.format;
		levels = desc.mipLevels;
	}
};

struct FramebufferDeko : Framebuffer {
	AutoRef<Image> color;
	AutoRef<Image> depth;
	std::string tag;
	FramebufferDeko(const FramebufferDesc &desc) : tag(desc.tag ? desc.tag : "Framebuffer") {
		width_ = desc.width;
		height_ = desc.height;
		layers_ = desc.numLayers;
		multiSampleLevel_ = desc.multiSampleLevel;
		hasDepthStencil_ = desc.z_stencil;
	}
	const char *Tag() const override { return tag.c_str(); }
};

struct BufferDeko : Buffer {
	std::vector<uint8_t> bytes;
	explicit BufferDeko(size_t size) : bytes(size) {}
};

struct Blend : BlendState {
	BlendStateDesc desc;
	explicit Blend(const BlendStateDesc &d) : desc(d) {}
};
struct Depth : DepthStencilState {
	DepthStencilStateDesc desc;
	explicit Depth(const DepthStencilStateDesc &d) : desc(d) {}
};
struct Raster : RasterState {
	RasterStateDesc desc;
	explicit Raster(const RasterStateDesc &d) : desc(d) {}
};
struct Layout : InputLayout {
	InputLayoutDesc desc;
	explicit Layout(const InputLayoutDesc &d) : desc(d) {}
};
struct Sampler : SamplerState {
	DkSamplerDescriptor descriptor{};
	uint64_t identity = 0;
};
struct Shader : ShaderModule {
	std::shared_ptr<Device> owner;
	MemoryPool::Slice code;
	DkShader shader{};
	ShaderStage stage;
	uint32_t arrays = 0;
	uint32_t textures = 0;
	uint32_t clipDistanceMode = 0;  // Deko3DClipDistanceMode() of a vertex shader.
	~Shader() override { owner->Retire(std::move(code)); }
	ShaderStage GetStage() const override { return stage; }
};
struct PipelineDeko : Pipeline {
	AutoRef<Blend> blend;
	AutoRef<Depth> depth;
	AutoRef<Raster> raster;
	AutoRef<Layout> layout;
	std::vector<AutoRef<Shader>> shaders;
	DkPrimitive primitive = DkPrimitive_Triangles;
	uint32_t arrays = 0;
	uint32_t textures = 0;
};

DkImageFormat Format(DataFormat format) {
	switch (format) {
	case DataFormat::R8_UNORM:
		return DkImageFormat_R8_Unorm;
	case DataFormat::R8G8_UNORM:
		return DkImageFormat_RG8_Unorm;
	case DataFormat::R16_UNORM:
		return DkImageFormat_R16_Unorm;
	case DataFormat::R16_FLOAT:
		return DkImageFormat_R16_Float;
	case DataFormat::R16G16B16A16_FLOAT:
		return DkImageFormat_RGBA16_Float;
	case DataFormat::R32_FLOAT:
		return DkImageFormat_R32_Float;
	case DataFormat::R5G6B5_UNORM_PACK16:
		return DkImageFormat_RGB565_Unorm;
	case DataFormat::A1R5G5B5_UNORM_PACK16:
		return DkImageFormat_RGB5A1_Unorm;
	case DataFormat::A4R4G4B4_UNORM_PACK16:
		return DkImageFormat_RGBA4_Unorm;
	case DataFormat::BC4_UNORM_BLOCK:
		return DkImageFormat_R_BC4_Unorm;
	case DataFormat::BC5_UNORM_BLOCK:
		return DkImageFormat_RG_BC5_Unorm;
	case DataFormat::ASTC_4x4_UNORM_BLOCK:
		return DkImageFormat_RGBA_ASTC_4x4;
	case DataFormat::ETC2_R8G8B8_UNORM_BLOCK:
		return DkImageFormat_RGB_ETC2;
	case DataFormat::ETC2_R8G8B8A1_UNORM_BLOCK:
		return DkImageFormat_RGB_PTA_ETC2;
	case DataFormat::ETC2_R8G8B8A8_UNORM_BLOCK:
		return DkImageFormat_RGBA_ETC2;
	case DataFormat::R8G8B8A8_UNORM:
		return DkImageFormat_RGBA8_Unorm;
	case DataFormat::B8G8R8A8_UNORM:
		return DkImageFormat_BGRA8_Unorm;
	case DataFormat::D16:
		return DkImageFormat_Z16;
	case DataFormat::D24_S8:
		return DkImageFormat_Z24S8;
	case DataFormat::D32F:
		return DkImageFormat_ZF32;
	case DataFormat::BC1_RGBA_UNORM_BLOCK:
		return DkImageFormat_RGBA_BC1;
	case DataFormat::BC2_UNORM_BLOCK:
		return DkImageFormat_RGBA_BC2;
	case DataFormat::BC3_UNORM_BLOCK:
		return DkImageFormat_RGBA_BC3;
	case DataFormat::BC7_UNORM_BLOCK:
		return DkImageFormat_RGBA_BC7_Unorm;
	default:
		return DkImageFormat_None;
	}
}

DkBlendFactor Factor(BlendFactor factor) {
	static constexpr DkBlendFactor values[] = {
		DkBlendFactor_Zero,		  DkBlendFactor_One,		   DkBlendFactor_SrcColor,	 DkBlendFactor_InvSrcColor,
		DkBlendFactor_DstColor,	  DkBlendFactor_InvDstColor,   DkBlendFactor_SrcAlpha,	 DkBlendFactor_InvSrcAlpha,
		DkBlendFactor_DstAlpha,	  DkBlendFactor_InvDstAlpha,   DkBlendFactor_ConstColor, DkBlendFactor_InvConstColor,
		DkBlendFactor_ConstAlpha, DkBlendFactor_InvConstAlpha, DkBlendFactor_Src1Color,	 DkBlendFactor_InvSrc1Color,
		DkBlendFactor_Src1Alpha,  DkBlendFactor_InvSrc1Alpha,
	};
	return values[(unsigned)factor];
}

struct Upload {
	DkBufExtents gpu{};
	uint8_t *cpu = nullptr;
};

// Tracks graphics bindings emitted into the current command stream.
struct ResourceBindings {
	std::array<DkResHandle, MAX_TEXTURE_SLOTS> textures{};
	std::array<DkBufExtents, 8> uniforms{};
	bool texturesValid = false;

	void Invalidate() {
		texturesValid = false;
		uniforms.fill({});
	}

	void Apply(dk::CmdBuf cmd, const std::array<DkResHandle, MAX_TEXTURE_SLOTS> &handles, const std::array<Upload, 8> &uploads) {
		if (!texturesValid || textures != handles) {
			cmd.bindTextures(DkStage_Fragment, 0, handles);
			textures = handles;
			texturesValid = true;
		}
		for (unsigned i = 0; i < uploads.size(); ++i) {
			const auto &buffer = uploads[i].gpu;
			auto &bound = uniforms[i];
			if (buffer.addr && (buffer.addr != bound.addr || buffer.size != bound.size)) {
				cmd.bindUniformBuffer(DkStage_Vertex, i, buffer.addr, buffer.size);
				cmd.bindUniformBuffer(DkStage_Fragment, i, buffer.addr, buffer.size);
				bound = buffer;
			}
		}
	}
};

struct Frame {
	dk::UniqueCmdBuf commands;
	DkFence fence{};
	bool submitted = false;
	std::vector<dk::UniqueMemBlock> commandMemory;
	size_t commandChunk = 0;
	std::vector<dk::UniqueMemBlock> uploadMemory;
	size_t uploadChunk = 0;
	uint32_t uploadOffset = 0;
	dk::UniqueMemBlock descriptors;
	uint32_t imageCount = 0, samplerCount = 0;
	std::unordered_map<uint64_t, uint32_t> imageCache;
	std::unordered_map<uint64_t, uint32_t> samplerCache;
	std::vector<RetiredMemory> retired;
};

class Context : public DrawContext {
public:
	~Context() override;
	bool Init(void *window, std::string *error);
	void Resize();
	void StopThreads() override;
	void StartThreads() override;
	void GenerateMips(Texture *texture, int firstLevel);
	void BindUniform(unsigned binding, const void *data, size_t size);
	void DepthClamp(bool enabled) {
		if (depthClamp_ != enabled) {
			depthClamp_ = enabled;
			stateDirty_ = true;
		}
	}
	void SetRaster(CullMode cull, bool clamp) {
		if (!overrideRaster_ || cullOverride_ != cull) {
			stateDirty_ = true;
		}
		cullOverride_ = cull;
		overrideRaster_ = true;
		DepthClamp(clamp);
	}
	Deko3DUpload Stream(uint32_t size, uint32_t alignment) {
		Upload u = Allocate(size, alignment);
		return {u.gpu.addr, u.gpu.size, u.cpu};
	}
	void DrawNative(Primitive primitive, uint64_t vertices, uint32_t size, int count, const uint16_t *indices);
	bool ReadTexture(Texture *texture, int level, void *pixels, int stride);
	SamplerState *SamplerWithLod(const SamplerStateDesc &desc, float minLod, float maxLod, float bias);
	bool DispatchScale(ShaderModule *shader, Deko3DUpload input, Texture *source, Texture *target, int level, const void *params,
					   size_t paramSize, unsigned groupsX, unsigned groupsY, const void *constants, size_t constantSize);

	const DeviceCaps &GetDeviceCaps() const override { return caps_; }
	uint32_t GetDataFormatSupport(DataFormat format) const override;
	uint32_t GetSupportedShaderLanguages() const override { return GLSL_VULKAN; }
	DepthStencilState *CreateDepthStencilState(const DepthStencilStateDesc &desc) override { return new Depth(desc); }
	BlendState *CreateBlendState(const BlendStateDesc &desc) override { return new Blend(desc); }
	SamplerState *CreateSamplerState(const SamplerStateDesc &desc) override { return SamplerWithLod(desc, 0, 1000, 0); }
	RasterState *CreateRasterState(const RasterStateDesc &desc) override { return new Raster(desc); }
	InputLayout *CreateInputLayout(const InputLayoutDesc &desc) override { return new Layout(desc); }
	ShaderModule *CreateShaderModule(ShaderStage stage, ShaderLanguage language, const uint8_t *data, size_t size,
									 const char *tag) override {
		// Shared thin3d shaders reserve Vulkan binding 0 for their uniform buffer.
		return CreateNativeShader(stage, language, data, size, tag, stage == ShaderStage::Compute ? 0 : 1);
	}
	ShaderModule *CreateNativeShader(ShaderStage stage, ShaderLanguage language, const uint8_t *data, size_t size, const char *tag,
									 uint32_t textureBindingBase);
	Pipeline *CreateGraphicsPipeline(const PipelineDesc &desc, const char *tag) override;
	Buffer *CreateBuffer(size_t size, uint32_t usage) override { return new BufferDeko(size); }
	Texture *CreateTexture(const TextureDesc &desc) override;
	Texture *CreateImage(const TextureDesc &desc, bool render, bool zeroFill = false);
	Framebuffer *CreateFramebuffer(const FramebufferDesc &desc) override;
	void UpdateBuffer(Buffer *buffer, const uint8_t *data, size_t offset, size_t size, UpdateBufferFlags flags) override;
	void UpdateTextureLevels(Texture *texture, const uint8_t **data, TextureCallback callback, int levels) override;
	void UpdateTextureRegions(Texture *texture, int level, const TextureRegionUpdate *regions, int count) override;
	void CopyFramebufferImage(Framebuffer *src, int level, int x, int y, int z, Framebuffer *dst, int dstLevel, int dstX, int dstY,
							  int dstZ, int w, int h, int depth, Aspect aspects, const char *tag) override;
	bool BlitFramebuffer(Framebuffer *src, int sx1, int sy1, int sx2, int sy2, Framebuffer *dst, int dx1, int dy1, int dx2, int dy2,
						 Aspect aspects, FBBlitFilter filter, const char *tag) override;
	bool CopyFramebufferToMemory(Framebuffer *src, Aspect aspect, int x, int y, int w, int h, DataFormat format, void *pixels, int stride,
								 ReadbackMode mode, const char *tag) override;
	void BindFramebufferAsRenderTarget(Framebuffer *fbo, const RenderPassInfo &rp, const char *tag) override;
	void BindFramebufferAsTexture(Framebuffer *fbo, int binding, Aspect aspect, int layer) override;
	void GetFramebufferDimensions(Framebuffer *fbo, int *w, int *h) override;
	void SetScissorRect(int x, int y, int w, int h) override;
	void SetViewport(const Viewport &vp) override;
	void SetBlendFactor(float color[4]) override;
	void SetStencilParams(uint8_t ref, uint8_t write, uint8_t compare) override;
	void BindSamplerStates(int start, int count, SamplerState **states) override;
	void BindTextures(int start, int count, Texture **textures, TextureBindFlags flags) override;
	void BindVertexBuffer(Buffer *buffer, int offset) override {
		vertexRef_ = static_cast<BufferDeko *>(buffer);
		vertex_ = vertexRef_.ptr;
		vertexOffset_ = offset;
	}
	void BindIndexBuffer(Buffer *buffer, int offset) override {
		indexRef_ = static_cast<BufferDeko *>(buffer);
		index_ = indexRef_.ptr;
		indexOffset_ = offset;
	}
	void BindNativeTexture(int slot, void *texture) override {
		if ((unsigned)slot < MAX_TEXTURE_SLOTS) {
			textures_[slot] = static_cast<Image *>(texture);
			textureRefs_[slot] = textures_[slot];
			textureAspects_[slot] = Aspect::COLOR_BIT;
		}
	}
	void UpdateDynamicUniformBuffer(const void *data, size_t size) override { BindUniform(0, data, size); }
	void Invalidate(InvalidationFlags flags) override {
		stateDirty_ = true;
		resourceBindings_.Invalidate();
	}
	void BindPipeline(Pipeline *pipeline) override {
		if (pipeline_ != pipeline) {
			overrideRaster_ = false;
			pipelineRef_ = static_cast<PipelineDeko *>(pipeline);
			pipeline_ = pipelineRef_.ptr;
			stateDirty_ = true;
		}
	}
	void Draw(int count, int offset) override;
	void DrawIndexed(int count, int offset) override;
	void DrawUP(const void *vertices, int count) override;
	void DrawIndexedUP(const void *vertices, int vertexCount, const void *indices, int indexCount) override;
	void DrawIndexedClippedBatchUP(const void *vertices, int vertexCount, const void *indices, int indexCount, Slice<ClippedDraw> draws,
								   const void *uniforms, size_t size) override;
	void BeginFrame(DebugFlags flags) override;
	void EndFrame() override;
	void Present(PresentMode mode) override;
	PresentMode GetCurrentPresentMode() const override { return presentMode_; }
	void Clear(Aspect aspects, uint32_t color, float depth, int stencil) override;
	std::string GetInfoString(InfoField info) const override;
	uint64_t GetNativeObject(NativeObject object, void *source) override;
	void HandleEvent(Event event, int w, int h, void *, void *) override;
	void SetInvalidationCallback(InvalidationCallback callback) override { invalidated_ = std::move(callback); }
	int GetFrameCount() override { return frameCount_; }
	void Wait() override;

private:
	Frame &Current() { return frames_[frameIndex_]; }
	dk::CmdBuf Commands() {
		if (!active_) {
			BeginFrame(DebugFlags::NONE);
		}
		return Current().commands;
	}
	Upload Allocate(uint32_t size, uint32_t alignment = 256);
	Upload Push(const void *data, uint32_t size, uint32_t alignment = 256);
	void Submit(bool wait);
	void CollectRetired();
	void ResetRenderBindings();
	void UpdateFrameGeneration();
	bool PresentImage(const DkImage &image, uint32_t width, uint32_t height);
	bool ReportQueueFault(const char *operation);
	bool Apply();
	Image *Color(Framebuffer *fbo);
	Image *DepthImage(Framebuffer *fbo);
	bool Swapchain(std::string *error);
	static void GrowCommands(void *user, DkCmdBuf buffer, size_t minimum);
	std::shared_ptr<Device> device_;
	std::array<Frame, FRAME_COUNT> frames_;
	unsigned frameIndex_ = 0;
	int frameCount_ = 0;
	bool reportedQueueFault_ = false;
	bool active_ = false, stateDirty_ = true, depthClamp_ = false;
	bool resourcesStopped_ = false;
	bool glslangInitialized_ = false;
	void *window_ = nullptr;
	dk::UniqueSwapchain swapchain_;
	std::array<AutoRef<Image>, 3> backbuffers_;
	AutoRef<Image> backDepth_;
	AutoRef<Image> generatedDisplay_;
	std::unique_ptr<Deko3DLSFG::FrameGenerator> frameGenerator_;
	Deko3DLSFG::FramePacer framePacer_;
	Deko3DLSFG::Config frameGenerationConfig_;
	SwitchLSFGDekoState frameGenerationState_;
	std::future<Deko3DLSFG::PreparedShaders> framePreparation_;
	std::thread framePreparationThread_;
	std::shared_ptr<std::atomic<bool>> framePreparationCancel_;
	std::string frameShaderDirectory_;
	bool framePreparationPerformance_ = true, framePreparationStarted_ = false, frameGenerationFailed_ = false;
	bool initialized_ = false;
	bool displayUsed_ = false;
	int frameGenerationDecision_ = -1;
	AutoRef<Image> nullTexture_;
	AutoRef<Sampler> defaultSampler_;
	int acquired_ = -1;
	PresentMode presentMode_ = PresentMode::FIFO;
	DeviceCaps caps_{};
	Framebuffer *target_ = nullptr;
	PipelineDeko *pipeline_ = nullptr;
	AutoRef<PipelineDeko> pipelineRef_;
	BufferDeko *vertex_ = nullptr, *index_ = nullptr;
	AutoRef<BufferDeko> vertexRef_, indexRef_;
	int vertexOffset_ = 0, indexOffset_ = 0;
	std::array<Image *, MAX_TEXTURE_SLOTS> textures_{};
	std::array<AutoRef<Image>, MAX_TEXTURE_SLOTS> textureRefs_{};
	std::array<Aspect, MAX_TEXTURE_SLOTS> textureAspects_{};
	AutoRef<FramebufferDeko> targetRef_;
	std::array<Sampler *, MAX_TEXTURE_SLOTS> samplers_{};
	std::array<AutoRef<Sampler>, MAX_TEXTURE_SLOTS> samplerRefs_{};
	std::array<Upload, 8> uniforms_{};
	ResourceBindings resourceBindings_;
	InvalidationCallback invalidated_;
	CullMode cullOverride_ = CullMode::NONE;
	bool overrideRaster_ = false;
	bool targetBound_ = false;
	DkViewport viewport_{};
	DkScissor scissor_{};
	std::array<float, 4> blendConstant_{};
	std::array<uint8_t, 3> stencilParams_{};
	void RestoreDynamicState();
};

bool Context::Init(void *window, std::string *error) {
	window_ = window;
	device_ = std::make_shared<Device>();
	device_->device =
		dk::DeviceMaker{}.setCbDebug(DekoDebug).setFlags(DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginUpperLeft).create();
	if (!device_->device) {
		*error = "Could not create the deko3d device";
		return false;
	}
	device_->queue = dk::QueueMaker{device_->device}
						 .setFlags(DkQueueFlags_Graphics | DkQueueFlags_Compute)
						 .setPerWarpScratchMemorySize(SCRATCH_PER_WARP)
						 .create();
	if (!device_->queue) {
		*error = "Could not create the deko3d queue";
		return false;
	}
	for (Frame &frame : frames_) {
		DkCmdBufMaker maker{};
		dkCmdBufMakerDefaults(&maker, device_->device);
		maker.userData = this;
		maker.cbAddMem = GrowCommands;
		frame.commands = dk::UniqueCmdBuf{dkCmdBufCreate(&maker)};
		frame.commandMemory.push_back(
			device_->Allocate(COMMAND_CHUNK_SIZE, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "command buffer"));
		frame.descriptors =
			device_->Allocate(IMAGE_DESCRIPTORS * sizeof(DkImageDescriptor) + SAMPLER_DESCRIPTORS * sizeof(DkSamplerDescriptor),
							  DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "frame descriptors");
		if (!frame.commands || !frame.commandMemory.back() || !frame.descriptors) {
			*error = "Could not allocate deko3d frame resources";
			return false;
		}
	}
	shaderLanguageDesc_ = ShaderLanguageDesc(GLSL_VULKAN);
	caps_.vendor = GPUVendor::VENDOR_NVIDIA;
	caps_.deviceName = "Nintendo Switch Tegra X1 (deko3d)";
	caps_.maxTextureSize = 16384;
	// Clip and cull distances share eight slots. Apply() sets the hardware's cull mode, which deko3d doesn't.
	caps_.maxClipDistances = 8;
	caps_.maxCullDistances = 8;
	caps_.coordConvention = CoordConvention::Vulkan;
	caps_.preferredDepthBufferFormat = DataFormat::D24_S8;
	caps_.preferredShadowMapFormatLow = DataFormat::D16;
	caps_.preferredShadowMapFormatHigh = DataFormat::D32F;
	caps_.anisoSupported = caps_.dualSourceBlend = caps_.logicOpSupported = true;
	caps_.depthClampSupported = caps_.framebufferCopySupported = caps_.framebufferBlitSupported = true;
	caps_.fragmentShaderFullPrecisionFloat = caps_.fragmentShaderInt32Supported = caps_.fragmentShaderDepthWriteSupported = true;
	caps_.textureDepthSupported = caps_.blendMinMaxSupported = caps_.provokingVertexLast = true;
	caps_.texture3DSupported = caps_.textureSwizzleSupported = caps_.samplerLodControl = true;
	caps_.multiSampleLevelsMask = 1;
	caps_.verySlowShaderCompiler = true;
	caps_.framebufferDepthCopySupported = true;
	caps_.presentModesSupported = PresentMode::FIFO | PresentMode::IMMEDIATE;
	caps_.presentMaxInterval = 1;
	caps_.presentInstantModeChange = true;
	init_glslang();
	glslangInitialized_ = true;
	if (!Swapchain(error)) {
		return false;
	}
	SamplerStateDesc sampler{};
	sampler.wrapU = sampler.wrapV = sampler.wrapW = TextureAddressMode::CLAMP_TO_EDGE;
	defaultSampler_.reset(static_cast<Sampler *>(CreateSamplerState(sampler)));
	const uint32_t pixel = 0xffffffff;
	TextureDesc texture{};
	texture.type = TextureType::LINEAR2D;
	texture.format = DataFormat::R8G8B8A8_UNORM;
	texture.width = texture.height = texture.depth = texture.mipLevels = 1;
	texture.initData.push_back((const uint8_t *)&pixel);
	nullTexture_.reset(static_cast<Image *>(CreateTexture(texture)));
	if (!nullTexture_ || !defaultSampler_) {
		return false;
	}
	initialized_ = true;
	return true;
}

Context::~Context() {
	if (framePreparationCancel_) {
		framePreparationCancel_->store(true, std::memory_order_release);
	}
	if (framePreparationThread_.joinable()) {
		framePreparationThread_.join();
	}
	if (device_ && device_->queue) {
		if (!device_->queue.isInErrorState()) {
			Submit(true);
		}
		if (device_->queue.isInErrorState()) {
			device_->queue = nullptr;
		} else {
			device_->queue.waitIdle();
		}
	}
	DestroyPresets();
	frameGenerator_.reset();
	generatedDisplay_.reset(nullptr);
	SwitchLSFG_SetDekoStatus(false, false);
	nullTexture_.reset(nullptr);
	defaultSampler_.reset(nullptr);
	backDepth_.reset(nullptr);
	for (auto &image : backbuffers_) {
		image.reset(nullptr);
	}
	swapchain_ = nullptr;
	for (Frame &frame : frames_) {
		frame.retired.clear();
	}
	if (glslangInitialized_) {
		finalize_glslang();
	}
}

void Context::GrowCommands(void *user, DkCmdBuf buffer, size_t minimum) {
	auto *context = static_cast<Context *>(user);
	Frame &frame = context->Current();
	++frame.commandChunk;
	if (frame.commandChunk >= frame.commandMemory.size()) {
		frame.commandMemory.push_back(
			context->device_->Allocate(std::max(COMMAND_CHUNK_SIZE, Align((uint32_t)minimum, DK_MEMBLOCK_ALIGNMENT)),
									   DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "command buffer"));
	}
	auto &memory = frame.commandMemory[frame.commandChunk];
	if (memory) {
		dkCmdBufAddMemory(buffer, memory, 0, dkMemBlockGetSize(memory));
	}
}

void Context::BeginFrame(DebugFlags flags) {
	if (active_) {
		return;
	}
	if (initialized_) {
		UpdateFrameGeneration();
	}
	Frame &frame = Current();
	if (frame.submitted) {
		dkFenceWait(&frame.fence, -1);
		frame.submitted = false;
	}
	frame.retired.clear();
	if ((frameCount_ & 63) == 0) {
		device_->texturePool.TrimEmpty();
	}
	frame.commands.clear();
	resourceBindings_.Invalidate();
	frame.commandChunk = frame.uploadChunk = frame.uploadOffset = 0;
	frame.imageCount = frame.samplerCount = 0;
	frame.imageCache.clear();
	frame.samplerCache.clear();
	frame.commands.addMemory(frame.commandMemory[0], 0, dkMemBlockGetSize(frame.commandMemory[0]));
	frame.commands.bindImageDescriptorSet(frame.descriptors.getGpuAddr(), IMAGE_DESCRIPTORS);
	// deko3d's clip space Y points up. PPSSPP emits Vulkan coordinates.
	const DkViewportSwizzle swizzle{DkSwizzle_PositiveX, DkSwizzle_NegativeY, DkSwizzle_PositiveZ, DkSwizzle_PositiveW};
	frame.commands.setViewportSwizzles(0, swizzle);
	frame.commands.bindSamplerDescriptorSet(frame.descriptors.getGpuAddr() + IMAGE_DESCRIPTORS * sizeof(DkImageDescriptor),
											SAMPLER_DESCRIPTORS);
	uniforms_.fill({});
	displayUsed_ = false;
	active_ = true;
	stateDirty_ = true;
}

Upload Context::Allocate(uint32_t size, uint32_t alignment) {
	Commands();
	Frame &frame = Current();
	frame.uploadOffset = Align(frame.uploadOffset, alignment);
	if (frame.uploadChunk >= frame.uploadMemory.size()) {
		frame.uploadMemory.push_back(device_->Allocate(std::max(UPLOAD_CHUNK_SIZE, Align(size, DK_MEMBLOCK_ALIGNMENT)),
													   DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "upload chunk"));
	}
	if (!frame.uploadMemory[frame.uploadChunk]) {
		return {};
	}
	if (frame.uploadOffset + size > dkMemBlockGetSize(frame.uploadMemory[frame.uploadChunk])) {
		++frame.uploadChunk;
		frame.uploadOffset = 0;
		return Allocate(size, alignment);
	}
	auto &block = frame.uploadMemory[frame.uploadChunk];
	if (!block) {
		return {};
	}
	Upload result{{block.getGpuAddr() + frame.uploadOffset, size}, (uint8_t *)block.getCpuAddr() + frame.uploadOffset};
	frame.uploadOffset += size;
	return result;
}

Upload Context::Push(const void *data, uint32_t size, uint32_t alignment) {
	Upload result = Allocate(size, alignment);
	if (result.cpu && data) {
		memcpy(result.cpu, data, size);
	}
	return result;
}

void Context::Submit(bool wait) {
	if (!active_ || ReportQueueFault("submit")) {
		return;
	}
	Frame &frame = Current();
	frame.commands.signalFence(frame.fence, true);
	device_->queue.submitCommands(frame.commands.finishList());
	frame.submitted = true;
	{
		std::lock_guard<std::mutex> guard(device_->mutex);
		for (auto &block : device_->pending) {
			frame.retired.push_back(std::move(block));
		}
		device_->pending.clear();
	}
	if (wait) {
		// Submitting command lists only enqueues them. Fence waits do not kick
		// pending work to the GPU; presentation normally performs that flush.
		device_->queue.flush();
		dkFenceWait(&frame.fence, -1);
		if (ReportQueueFault("synchronous submission")) {
			return;
		}
		frame.submitted = false;
		frame.retired.clear();
		frame.commands.clear();
		resourceBindings_.Invalidate();
		frame.commandChunk = 0;
		frame.commands.addMemory(frame.commandMemory[0], 0, dkMemBlockGetSize(frame.commandMemory[0]));
		frame.commands.bindImageDescriptorSet(frame.descriptors.getGpuAddr(), IMAGE_DESCRIPTORS);
		// deko3d's clip space Y points up. PPSSPP emits Vulkan coordinates.
		const DkViewportSwizzle swizzle{DkSwizzle_PositiveX, DkSwizzle_NegativeY, DkSwizzle_PositiveZ, DkSwizzle_PositiveW};
		frame.commands.setViewportSwizzles(0, swizzle);
		frame.commands.bindSamplerDescriptorSet(frame.descriptors.getGpuAddr() + IMAGE_DESCRIPTORS * sizeof(DkImageDescriptor),
												SAMPLER_DESCRIPTORS);
		RestoreDynamicState();
		stateDirty_ = true;
		if (invalidated_) {
			invalidated_(InvalidationCallbackFlags::COMMAND_BUFFER_STATE | InvalidationCallbackFlags::RENDER_PASS_STATE);
		}
	}
}

void Context::CollectRetired() {
	// Call only after the entire queue is idle, with no new draws recorded.
	for (Frame &frame : frames_) {
		frame.submitted = false;
		frame.retired.clear();
	}
	{
		std::lock_guard<std::mutex> guard(device_->mutex);
		device_->pending.clear();
	}
	device_->texturePool.TrimEmpty();
}

void Context::Wait() {
	Submit(true);
	if (device_->queue && !device_->queue.isInErrorState()) {
		device_->queue.waitIdle();
		if (!device_->queue.isInErrorState()) {
			CollectRetired();
		}
	}
}

void Context::ResetRenderBindings() {
	resourceBindings_.Invalidate();
	target_ = nullptr;
	targetBound_ = false;
	targetRef_.reset(nullptr);
	textures_.fill(nullptr);
	for (auto &texture : textureRefs_) {
		texture.reset(nullptr);
	}
	stateDirty_ = true;
}

void Context::StopThreads() {
	// The common framebuffer manager brackets render-resolution changes with these hooks.
	ResetRenderBindings();
	Wait();
	framePacer_.Reset();
	if (frameGenerator_) {
		frameGenerator_->Reset();
	}
	resourcesStopped_ = true;
}

void Context::StartThreads() {
	if (resourcesStopped_ && !device_->queue.isInErrorState()) {
		// DestroyAllFBOs released more images after StopThreads drained the queue.
		CollectRetired();
	}
	resourcesStopped_ = false;
}

bool Context::ReportQueueFault(const char *operation) {
	if (!dkQueueIsInErrorState(device_->queue)) {
		return false;
	}
	if (reportedQueueFault_) {
		return true;
	}
	reportedQueueFault_ = true;
	ERROR_LOG(Log::G3D, "deko3d queue fault during %s", operation);
	System_ExitApp();
	return true;
}

void Context::UpdateFrameGeneration() {
	const auto state = SwitchLSFG_PollDeko();
	const Deko3DLSFG::Config config{state.performance, state.flowScale == .5f ? 2u : 4u};
	const bool qualityChanged = config.performance != frameGenerationConfig_.performance;
	const bool changed = qualityChanged || config.flowDenominator != frameGenerationConfig_.flowDenominator ||
						 state.enabled != frameGenerationState_.enabled;
	const bool speedChanged = state.speedControlState != frameGenerationState_.speedControlState;
	if (changed || frameGenerationFailed_) {
		if (frameGenerator_ || generatedDisplay_) {
			ResetRenderBindings();
			Wait();
			if (ReportQueueFault("LSFG resource change")) {
				SwitchLSFG_SetDekoStatus(false, false);
				return;
			}
			frameGenerator_.reset();
			generatedDisplay_.reset(nullptr);
			CollectRetired();
		}
	}
	if (changed) {
		framePacer_.Reset();
		frameGenerationDecision_ = -1;
		frameGenerationFailed_ = false;
	}
	if (qualityChanged || !state.enabled) {
		if (framePreparationCancel_) {
			framePreparationCancel_->store(true, std::memory_order_release);
		}
		framePreparationStarted_ = false;
		if (qualityChanged) {
			frameShaderDirectory_.clear();
		}
	}
	frameGenerationConfig_ = config;
	frameGenerationState_ = state;
	if (state.resetSession || speedChanged) {
		framePacer_.Reset();
		frameGenerationDecision_ = -1;
		if (state.resetSession) {
			frameGenerationFailed_ = false;
		}
		if (frameGenerator_) {
			frameGenerator_->Reset();
		}
	}
	if (framePreparation_.valid() && framePreparation_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		framePreparationThread_.join();
		auto prepared = framePreparation_.get();
		framePreparationCancel_.reset();
		if (!prepared.cancelled && framePreparationPerformance_ == config.performance) {
			if (prepared.error.empty()) {
				frameShaderDirectory_ = std::move(prepared.directory);
			} else if (state.enabled) {
				frameGenerationFailed_ = true;
				ERROR_LOG(Log::G3D, "LSFG shader preparation failed: %s", prepared.error.c_str());
			}
		}
	}
	if (!state.enabled || frameGenerationFailed_) {
		SwitchLSFG_SetDekoStatus(!frameGenerationFailed_, false);
		return;
	}
	if (!framePreparationStarted_ && frameShaderDirectory_.empty() && !framePreparation_.valid()) {
		framePreparationStarted_ = true;
		framePreparationPerformance_ = config.performance;
		framePreparationCancel_ = std::make_shared<std::atomic<bool>>(false);
		auto task = std::make_shared<std::packaged_task<Deko3DLSFG::PreparedShaders()>>(
			[performance = config.performance, cancel = framePreparationCancel_] {
				SetCurrentThreadName("DekoLSFGShaders");
				SetCurrentThreadAffinity(ThreadAffinityRole::BACKGROUND);
				return Deko3DLSFG::PrepareShaders(performance, *cancel);
			});
		framePreparation_ = task->get_future();
		framePreparationThread_ = std::thread([task] { (*task)(); });
	}
	if (!frameGenerator_ && !frameShaderDirectory_.empty()) {
		Deko3DLSFG::Services services;
		services.device = device_->device;
		services.scratchPerWarp = SCRATCH_PER_WARP;
		services.allocate = [device = device_](uint32_t bytes, uint32_t flags, const char *tag) {
			return device->Allocate(bytes, flags, tag);
		};
		services.retire = [device = device_](dk::UniqueMemBlock block) { device->Retire(std::move(block)); };
		services.log = [](const std::string &message) { INFO_LOG(Log::G3D, "%s", message.c_str()); };
		TextureDesc desc{};
		desc.type = TextureType::LINEAR2D;
		desc.width = primarySwapchain_->Width();
		desc.height = primarySwapchain_->Height();
		desc.depth = desc.mipLevels = 1;
		desc.format = DataFormat::R8G8B8A8_UNORM;
		frameGenerator_ = Deko3DLSFG::CreateFrameGenerator(std::move(services), config, frameShaderDirectory_, desc.width, desc.height);
		if (frameGenerator_) {
			generatedDisplay_.reset(static_cast<Image *>(CreateImage(desc, true, true)));
		}
		if (!frameGenerator_ || !generatedDisplay_) {
			frameGenerator_.reset();
			generatedDisplay_.reset(nullptr);
			frameGenerationFailed_ = true;
			ERROR_LOG(Log::G3D, "LSFG initialization failed");
		}
	}
	SwitchLSFG_SetDekoStatus(!frameGenerationFailed_, state.active && frameGenerationDecision_ == 2);
}

bool Context::PresentImage(const DkImage &image, uint32_t width, uint32_t height) {
	if (ReportQueueFault("LSFG acquire")) {
		return false;
	}
	if (acquired_ < 0) {
		acquired_ = device_->queue.acquireImage(swapchain_);
	}
	if (acquired_ < 0 || acquired_ >= (int)backbuffers_.size()) {
		return false;
	}
	DkImageView source, destination;
	dkImageViewDefaults(&source, &image);
	dkImageViewDefaults(&destination, &backbuffers_[acquired_]->image);
	const DkImageRect rect{0, 0, 0, width, height, 1};
	auto commands = Commands();
	commands.barrier(DkBarrier_Full, DkInvalidateFlags_Image);
	commands.blitImage(source, rect, destination, rect, DkBlitFlag_FilterNearest);
	Submit(false);
	if (ReportQueueFault("LSFG present")) {
		return false;
	}
	device_->queue.presentImage(swapchain_, acquired_);
	saltynx_frame(1);
	acquired_ = -1;
	return true;
}

void Context::EndFrame() {}

void Context::Present(PresentMode mode) {
	if (!active_ || ReportQueueFault("present")) {
		return;
	}
	presentMode_ = mode;
	if (generatedDisplay_ && displayUsed_) {
		const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		const auto decision = framePacer_.Observe(now, frameGenerationState_.active, frameGenerator_ != nullptr);
		const int pacing = decision.highRate ? 2 : decision.generate ? 1 : 0;
		if (frameGenerationState_.active && pacing != frameGenerationDecision_) {
			frameGenerationDecision_ = pacing;
		}
		if (decision.reset && frameGenerator_) {
			frameGenerator_->Reset();
		}
		bool generated = false;
		if (decision.generate && frameGenerator_) {
			resourceBindings_.Invalidate();
			generated =
				frameGenerator_->Generate(Commands(), generatedDisplay_->image, generatedDisplay_->Width(), generatedDisplay_->Height());
			frameGenerationFailed_ = frameGenerator_->Failed();
			stateDirty_ = true;
			targetBound_ = false;
			if (invalidated_) {
				invalidated_(InvalidationCallbackFlags::COMMAND_BUFFER_STATE | InvalidationCallbackFlags::RENDER_PASS_STATE);
			}
		}
		SwitchLSFG_SetDekoStatus(!frameGenerationFailed_, decision.highRate);
		swapchain_.setSwapInterval(generated || mode == PresentMode::FIFO ? 1 : 0);
		if (generated && !PresentImage(*frameGenerator_->GeneratedImage(), generatedDisplay_->Width(), generatedDisplay_->Height())) {
			return;
		}
		if (!PresentImage(generatedDisplay_->image, generatedDisplay_->Width(), generatedDisplay_->Height())) {
			return;
		}
	} else {
		swapchain_.setSwapInterval(mode == PresentMode::FIFO ? 1 : 0);
		Submit(false);
		if (ReportQueueFault("present")) {
			return;
		}
		if (acquired_ >= 0) {
			device_->queue.presentImage(swapchain_, acquired_);
			saltynx_frame(1);
			acquired_ = -1;
		} else {
			// Offscreen/upload-only frames have no present operation to flush them.
			device_->queue.flush();
		}
	}
	active_ = false;
	frameIndex_ = (frameIndex_ + 1) % FRAME_COUNT;
	++frameCount_;
}

uint32_t Context::GetDataFormatSupport(DataFormat format) const {
	uint32_t result = 0;
	if (Format(format) != DkImageFormat_None) {
		result = FMT_TEXTURE;
		if (format == DataFormat::D16 || format == DataFormat::D24_S8 || format == DataFormat::D32F) {
			result |= FMT_DEPTHSTENCIL;
		} else if (!DataFormatIsBlockCompressed(format, nullptr)) {
			result |= FMT_BLIT;
			if (format == DataFormat::R8G8B8A8_UNORM || format == DataFormat::B8G8R8A8_UNORM || format == DataFormat::R16G16B16A16_FLOAT) {
				result |= FMT_RENDERTARGET | FMT_AUTOGEN_MIPS;
			}
		}
	}
	switch (format) {
	case DataFormat::R32_FLOAT:
	case DataFormat::R32G32_FLOAT:
	case DataFormat::R32G32B32_FLOAT:
	case DataFormat::R32G32B32A32_FLOAT:
	case DataFormat::R8G8B8A8_UNORM:
	case DataFormat::R8G8B8A8_SNORM:
	case DataFormat::R16G16_UNORM:
	case DataFormat::R16G16B16A16_UNORM:
	case DataFormat::R16G16B16A16_SNORM:
		result |= FMT_INPUTLAYOUT;
		break;
	default:
		break;
	}
	return result;
}

Texture *Context::CreateTexture(const TextureDesc &desc) { return CreateImage(desc, false); }

Texture *Context::CreateImage(const TextureDesc &desc, bool render, bool zeroFill) {
	if (desc.width <= 0 || desc.height <= 0 || desc.depth <= 0 || desc.width > 16384 || desc.height > 16384 || desc.depth > 512 ||
		desc.mipLevels < 1 || desc.mipLevels > 15 || Format(desc.format) == DkImageFormat_None) {
		return nullptr;
	}
	std::unique_ptr<Image, void (*)(Image *)> texture(new Image(), [](Image *image) { image->Release(); });
	texture->owner = device_;
	texture->Dimensions(desc);
	texture->identity = device_->nextImage++;
	dk::ImageLayout layout;
	uint32_t flags = 0;
	if (!DataFormatIsBlockCompressed(desc.format, nullptr)) {
		flags |= DkImageFlags_Usage2DEngine;
	}
	if (desc.format == DataFormat::R8G8B8A8_UNORM || desc.format == DataFormat::R16G16B16A16_FLOAT) {
		flags |= DkImageFlags_UsageLoadStore;
	}
	if (render) {
		flags |= DkImageFlags_UsageRender | DkImageFlags_HwCompression;
	}
	dk::ImageLayoutMaker{device_->device}
		.setType(desc.depth > 1 ? DkImageType_3D : DkImageType_2D)
		.setFlags(flags)
		.setFormat(Format(desc.format))
		.setDimensions(desc.width, desc.height, desc.depth)
		.setMipLevels(desc.mipLevels)
		.initialize(layout);
	if (layout.getSize() > 512 * 1024 * 1024) {
		return nullptr;
	}
	constexpr uint32_t imageFlags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
	const uint32_t size = (uint32_t)layout.getSize();
	const uint32_t alignment = layout.getAlignment();
	// A 512-byte layout uses a generic mapping. Other layouts can retype whole GPU pages.
	if (!render && !zeroFill && alignment == 512 && size <= TEXTURE_SLAB_SIZE / 2) {
		texture->pooledMemory = device_->texturePool.Allocate(size, alignment, TEXTURE_SLAB_SIZE, [&](uint32_t bytes) {
			return device_->Allocate(bytes, imageFlags, "texture slab");
		});
		if (!texture->pooledMemory) {
			return nullptr;
		}
		texture->image.initialize(layout, texture->pooledMemory.Storage(), texture->pooledMemory.Offset());
	} else {
		texture->memory = device_->Allocate(size, imageFlags | (zeroFill ? DkMemBlockFlags_ZeroFillInit : 0),
										   render ? "render target" : "texture");
		if (!texture->memory) {
			return nullptr;
		}
		texture->image.initialize(layout, texture->memory, 0);
	}
	dkImageViewDefaults(&texture->view, &texture->image);
	if (desc.swizzle != TextureSwizzle::DEFAULT) {
		texture->view.swizzle[3] = desc.swizzle == TextureSwizzle::R8_AS_GRAYSCALE ? DkImageSwizzle_One : DkImageSwizzle_Red;
		for (int i = 0; i < 3; ++i) {
			texture->view.swizzle[i] = desc.swizzle == TextureSwizzle::R8_AS_ALPHA ? DkImageSwizzle_One : DkImageSwizzle_Red;
		}
	}
	if (!desc.initData.empty()) {
		std::vector<const uint8_t *> data = desc.initData;
		UpdateTextureLevels(texture.get(), data.data(), desc.initDataCallback, (int)data.size());
	}
	if (desc.generateMips) {
		GenerateMips(texture.get(), (int)desc.initData.size());
	}
	return texture.release();
}

void Context::GenerateMips(Texture *source, int firstLevel) {
	auto *texture = static_cast<Image *>(source);
	if (texture->Depth() == 1 && firstLevel > 0) {
		for (int level = firstLevel; level < texture->levels; ++level) {
			DkImageView from = texture->view, to = texture->view;
			from.mipLevelOffset = level - 1;
			from.mipLevelCount = 1;
			to.mipLevelOffset = level;
			to.mipLevelCount = 1;
			DkImageRect source{
				0, 0, 0, (uint32_t)std::max(1, texture->Width() >> (level - 1)), (uint32_t)std::max(1, texture->Height() >> (level - 1)),
				1};
			DkImageRect target{0, 0, 0, (uint32_t)std::max(1, texture->Width() >> level), (uint32_t)std::max(1, texture->Height() >> level),
							   1};
			Commands().barrier(DkBarrier_Primitives, DkInvalidateFlags_Image);
			Commands().blitImage(from, source, to, target, DkBlitFlag_FilterLinear);
		}
		Commands().barrier(DkBarrier_None, DkInvalidateFlags_Image);
	}
}

Framebuffer *Context::CreateFramebuffer(const FramebufferDesc &desc) {
	if (desc.numLayers != 1 || desc.multiSampleLevel != 0) {
		return nullptr;
	}
	std::unique_ptr<FramebufferDeko, void (*)(FramebufferDeko *)> framebuffer(new FramebufferDeko(desc),
																			  [](FramebufferDeko *fb) { fb->Release(); });
	TextureDesc texture{};
	texture.type = TextureType::LINEAR2D;
	texture.width = desc.width;
	texture.height = desc.height;
	texture.depth = texture.mipLevels = 1;
	texture.format = DataFormat::R8G8B8A8_UNORM;
	framebuffer->color.reset(static_cast<Image *>(CreateImage(texture, true)));
	if (!framebuffer->color) {
		return nullptr;
	}
	if (desc.z_stencil) {
		texture.format = DataFormat::D24_S8;
		framebuffer->depth.reset(static_cast<Image *>(CreateImage(texture, true)));
		if (!framebuffer->depth) {
			return nullptr;
		}
	}
	return framebuffer.release();
}

void Context::UpdateBuffer(Buffer *buffer, const uint8_t *data, size_t offset, size_t size, UpdateBufferFlags flags) {
	auto *target = static_cast<BufferDeko *>(buffer);
	if (offset <= target->bytes.size() && size <= target->bytes.size() - offset) {
		memcpy(target->bytes.data() + offset, data, size);
	}
}

void Context::UpdateTextureLevels(Texture *source, const uint8_t **data, TextureCallback callback, int levels) {
	auto *texture = static_cast<Image *>(source);
	for (int level = 0; level < std::min(levels, texture->levels); ++level) {
		const uint32_t w = std::max(1, texture->Width() >> level), h = std::max(1, texture->Height() >> level),
					   d = std::max(1, texture->Depth() >> level);
		const uint32_t size = (uint32_t)DataFormatSizeInBytes(texture->Format());
		int block = 1;
		const bool compressed = DataFormatIsBlockCompressed(texture->Format(), &block);
		const uint32_t rowPitch = compressed ? ((w + 3) / 4) * block : w * size;
		const uint32_t slicePitch = rowPitch * (compressed ? (h + 3) / 4 : h);
		Upload upload = Allocate(slicePitch * d);
		if (!upload.cpu) {
			return;
		}
		if (callback) {
			if (!callback(upload.cpu, data[level], w, h, d, rowPitch, slicePitch)) {
				continue;
			}
		} else if (data[level]) {
			memcpy(upload.cpu, data[level], slicePitch * d);
		} else {
			memset(upload.cpu, 0, slicePitch * d);
		}
		DkImageView view = texture->view;
		view.mipLevelOffset = level;
		view.mipLevelCount = 1;
		// DkCopyBuf pitches are byte counts, not pixel dimensions. These rows
		// and slices are tightly packed, so use the driver's derived pitches.
		DkCopyBuf buffer{upload.gpu.addr, 0, 0};
		DkImageRect region{0, 0, 0, w, h, d};
		Commands().copyBufferToImage(buffer, view, region);
	}
	Commands().barrier(DkBarrier_None, DkInvalidateFlags_Image);
}

void Context::UpdateTextureRegions(Texture *source, int level, const TextureRegionUpdate *regions, int count) {
	auto *texture = static_cast<Image *>(source);
	const uint32_t pixelSize = (uint32_t)DataFormatSizeInBytes(texture->Format());
	DkImageView view = texture->view;
	view.mipLevelOffset = level;
	view.mipLevelCount = 1;
	for (int i = 0; i < count; ++i) {
		const auto &region = regions[i];
		Upload upload = Allocate(region.w * region.h * pixelSize);
		if (!upload.cpu) {
			return;
		}
		for (int y = 0; y < region.h; ++y) {
			memcpy(upload.cpu + y * region.w * pixelSize, region.data + y * (region.byteStride ? region.byteStride : region.w * pixelSize),
				   region.w * pixelSize);
		}
		DkCopyBuf buffer{upload.gpu.addr, 0, 0};
		DkImageRect rect{(uint32_t)region.x, (uint32_t)region.y, 0, (uint32_t)region.w, (uint32_t)region.h, 1};
		Commands().copyBufferToImage(buffer, view, rect);
	}
	Commands().barrier(DkBarrier_None, DkInvalidateFlags_Image);
}

SamplerState *Context::SamplerWithLod(const SamplerStateDesc &desc, float minLod, float maxLod, float bias) {
	static const DkWrapMode wrap[] = {DkWrapMode_Repeat, DkWrapMode_MirroredRepeat, DkWrapMode_ClampToEdge, DkWrapMode_ClampToBorder};
	DkSampler sampler{};
	dkSamplerDefaults(&sampler);
	sampler.minFilter = desc.minFilter == TextureFilter::LINEAR ? DkFilter_Linear : DkFilter_Nearest;
	sampler.magFilter = desc.magFilter == TextureFilter::LINEAR ? DkFilter_Linear : DkFilter_Nearest;
	sampler.mipFilter = desc.mipFilter == TextureFilter::LINEAR ? DkMipFilter_Linear : DkMipFilter_Nearest;
	sampler.wrapMode[0] = wrap[(int)desc.wrapU];
	sampler.wrapMode[1] = wrap[(int)desc.wrapV];
	sampler.wrapMode[2] = wrap[(int)desc.wrapW];
	sampler.lodClampMin = minLod;
	sampler.lodClampMax = maxLod;
	sampler.lodBias = bias;
	sampler.maxAnisotropy = std::clamp(desc.maxAniso, 1.f, 16.f);
	sampler.compareEnable = desc.shadowCompareEnabled;
	sampler.compareOp = (DkCompareOp)((int)desc.shadowCompareFunc + 1);
	if (desc.borderColor == OPAQUE_WHITE) {
		for (auto &value : sampler.borderColor) {
			value.value_f = 1.f;
		}
	} else if (desc.borderColor == OPAQUE_BLACK) {
		sampler.borderColor[3].value_f = 1.f;
	}
	auto *result = new Sampler();
	result->identity = device_->nextSampler++;
	dkSamplerDescriptorInitialize(&result->descriptor, &sampler);
	return result;
}

ShaderModule *Context::CreateNativeShader(ShaderStage stage, ShaderLanguage language, const uint8_t *data, size_t size, const char *tag,
										  uint32_t textureBindingBase) {
	if (language != GLSL_VULKAN || (stage != ShaderStage::Vertex && stage != ShaderStage::Fragment && stage != ShaderStage::Compute)) {
		return nullptr;
	}
	// UAM and glslang both have process-wide compiler state. No graphics queue work is
	// performed while holding this lock.
	static std::mutex compilerMutex;
	std::lock_guard<std::mutex> guard(compilerMutex);
	std::string source((const char *)data, size), error;
	struct CacheHeader {
		uint32_t magic, version, stage, arrays, textures, binarySize;
		uint8_t sourceHash[20], binaryHash[20];
	} cache{};
	cache.magic = 0x434B4544;
	cache.version = 2;
	cache.stage = (uint32_t)stage;
	std::string identity = "uam-c0d76aa9-ppsspp-5/" + StringFromInt((int)stage) + "/" + StringFromInt(textureBindingBase) + "/" + source;
	sha1((uint8_t *)identity.data(), (int)identity.size(), cache.sourceHash);
	std::string key;
	for (uint8_t byte : cache.sourceHash) {
		key += StringFromFormat("%02x", byte);
	}
	const Path directory("/switch/ppsspp-nx/cache/deko3d");
	const Path cachePath = directory / (key + ".dksh");
	std::string compiledBytes;
	uint32_t arrays = 0, textures = 0;
	bool fromCache = false;
	if (g_Config.bShaderCache) {
		FILE *file = File::OpenCFile(cachePath, "rb");
		if (file) {
			CacheHeader saved{};
			if (fread(&saved, sizeof(saved), 1, file) == 1 && saved.magic == cache.magic && saved.version == cache.version &&
				saved.stage == cache.stage && !memcmp(saved.sourceHash, cache.sourceHash, 20) && saved.binarySize >= 24 &&
				saved.binarySize <= 2 * 1024 * 1024) {
				compiledBytes.resize(saved.binarySize);
				uint8_t hash[20]{};
				if (fread(compiledBytes.data(), 1, compiledBytes.size(), file) == compiledBytes.size() && fgetc(file) == EOF) {
					sha1_context digest;
					sha1_starts(&digest);
					sha1_update(&digest, (uint8_t *)&saved, offsetof(CacheHeader, binaryHash));
					sha1_update(&digest, (uint8_t *)compiledBytes.data(), (int)compiledBytes.size());
					sha1_finish(&digest, hash);
					fromCache = memcmp(hash, saved.binaryHash, 20) == 0;
				}
				if (fromCache) {
					arrays = saved.arrays;
					textures = saved.textures;
				}
			}
			fclose(file);
		}
	}
	size_t binarySize = 0;
	// The cache doesn't keep the distance counts, so they come from the source, checked against the compiler
	// whenever it runs.
	uint32_t clipDistances = 0, cullDistances = 0, compiledClip = 0, compiledCull = 0;
	Deko3DCountDistances(source.c_str(), &clipDistances, &cullDistances);
	if (!fromCache && !CompileDeko3DShader(stage, source.c_str(), &compiledBytes, &arrays, &textures, &error, textureBindingBase,
										   &compiledClip, &compiledCull)) {
		ERROR_LOG(Log::G3D, "deko3d compiler %s: %s", tag, error.c_str());
		return nullptr;
	}
	if (!fromCache && stage == ShaderStage::Vertex && (compiledClip != clipDistances || compiledCull != cullDistances)) {
		const std::string detail = StringFromFormat("source clip=%u cull=%u compiled clip=%u cull=%u", clipDistances, cullDistances,
													compiledClip, compiledCull);
		ERROR_LOG(Log::G3D, "deko3d shader %s distance counts differ: %s", tag, detail.c_str());
		clipDistances = compiledClip;
		cullDistances = compiledCull;
	}
	binarySize = compiledBytes.size();
	const void *binary = compiledBytes.data();
	struct Header {
		uint32_t magic, headerSize, controlSize, codeSize, programsOffset, programs;
	} header{};
	if (binarySize < sizeof(header)) {
		return nullptr;
	}
	memcpy(&header, binary, sizeof(header));
	if (header.magic != 0x48534b44 || header.headerSize != sizeof(Header) || header.controlSize < sizeof(Header) || !header.codeSize ||
		header.codeSize > 2 * 1024 * 1024 || header.controlSize > binarySize || header.codeSize > binarySize - header.controlSize ||
		header.programs != 1 || header.programsOffset > header.controlSize || header.controlSize - header.programsOffset < 64) {
		return nullptr;
	}
	uint32_t scratch = 0;
	memcpy(&scratch, (const uint8_t *)binary + header.programsOffset + 20, 4);
	if (scratch > SCRATCH_PER_WARP) {
		ERROR_LOG(Log::G3D, "deko3d shader %s needs too much scratch memory", tag);
		return nullptr;
	}
	std::unique_ptr<Shader, void (*)(Shader *)> shader(new Shader(), [](Shader *s) { s->Release(); });
	shader->owner = device_;
	shader->stage = stage;
	shader->arrays = arrays;
	shader->textures = textures;
	if (stage == ShaderStage::Vertex) {
		shader->clipDistanceMode = Deko3DClipDistanceMode(clipDistances, cullDistances);
	}
	const uint32_t bytes = header.codeSize + DK_SHADER_CODE_UNUSABLE_SIZE;
	const uint32_t slabSize = std::max(SHADER_SLAB_SIZE, Align(bytes, DK_MEMBLOCK_ALIGNMENT));
	// Keep empty code slabs available for reuse. deko3d does not reclaim their code address ranges.
	shader->code = device_->shaderPool.Allocate(bytes, DK_SHADER_CODE_ALIGNMENT, slabSize, [&](uint32_t slabBytes) {
		return device_->Allocate(slabBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
								 "shader code slab");
	});
	if (!shader->code) {
		ERROR_LOG(Log::G3D, "deko3d shader %s: code allocation failed", tag);
		return nullptr;
	}
	const auto &code = shader->code;
	memcpy((uint8_t *)dkMemBlockGetCpuAddr(code.Storage()) + code.Offset(), (const uint8_t *)binary + header.controlSize, header.codeSize);
	DkShaderMaker maker{};
	dkShaderMakerDefaults(&maker, code.Storage(), code.Offset());
	maker.control = binary;
	dkShaderInitialize(&shader->shader, &maker);
	const DkStage expectedStage = stage == ShaderStage::Vertex	   ? DkStage_Vertex
								  : stage == ShaderStage::Fragment ? DkStage_Fragment
																   : DkStage_Compute;
	if (!dkShaderIsValid(&shader->shader) || dkShaderGetStage(&shader->shader) != expectedStage) {
		ERROR_LOG(Log::G3D, "deko3d rejected shader %s", tag);
		return nullptr;
	}
	device_->shaderCodeDirty.store(true, std::memory_order_release);
	if (!fromCache && g_Config.bShaderCache && File::CreateFullPath(directory)) {
		cache.arrays = arrays;
		cache.textures = textures;
		cache.binarySize = (uint32_t)binarySize;
		sha1_context digest;
		sha1_starts(&digest);
		sha1_update(&digest, (uint8_t *)&cache, offsetof(CacheHeader, binaryHash));
		sha1_update(&digest, (uint8_t *)compiledBytes.data(), (int)compiledBytes.size());
		sha1_finish(&digest, cache.binaryHash);
		const Path temporary(cachePath.ToString() + ".tmp");
		FILE *file = File::OpenCFile(temporary, "wb");
		if (file) {
			bool ok = fwrite(&cache, sizeof(cache), 1, file) == 1 && fwrite(binary, 1, binarySize, file) == binarySize;
			ok = fclose(file) == 0 && ok;
			if (ok) {
				File::Rename(temporary, cachePath);
			} else {
				File::Delete(temporary);
			}
		}
	}
	return shader.release();
}

Pipeline *Context::CreateGraphicsPipeline(const PipelineDesc &desc, const char *tag) {
	if (!desc.blend || !desc.depthStencil || !desc.raster || !desc.inputLayout || desc.shaders.size() < 2) {
		return nullptr;
	}
	static const DkPrimitive primitives[] = {DkPrimitive_Points,	DkPrimitive_Lines,		   DkPrimitive_LineStrip,
											 DkPrimitive_Triangles, DkPrimitive_TriangleStrip, DkPrimitive_TriangleFan};
	if ((unsigned)desc.prim >= std::size(primitives)) {
		return nullptr;
	}
	auto *pipeline = new PipelineDeko();
	pipeline->blend = static_cast<Blend *>(desc.blend);
	pipeline->depth = static_cast<Depth *>(desc.depthStencil);
	pipeline->raster = static_cast<Raster *>(desc.raster);
	pipeline->layout = static_cast<Layout *>(desc.inputLayout);
	pipeline->primitive = primitives[(unsigned)desc.prim];
	for (ShaderModule *module : desc.shaders) {
		pipeline->shaders.emplace_back(static_cast<Shader *>(module));
		pipeline->arrays |= static_cast<Shader *>(module)->arrays;
		pipeline->textures |= static_cast<Shader *>(module)->textures;
	}
	return pipeline;
}

void Context::BindUniform(unsigned binding, const void *data, size_t size) {
	if (binding >= uniforms_.size() || size > 65536) {
		return;
	}
	Upload upload = Allocate(Align((uint32_t)size, 256));
	if (!upload.cpu) {
		return;
	}
	memset(upload.cpu, 0, upload.gpu.size);
	memcpy(upload.cpu, data, size);
	uniforms_[binding] = upload;
}

void Context::BindTextures(int start, int count, Texture **textures, TextureBindFlags flags) {
	for (int i = 0; i < count && start + i < MAX_TEXTURE_SLOTS; ++i) {
		textures_[start + i] = static_cast<Image *>(textures[i]);
		textureRefs_[start + i] = textures_[start + i];
		textureAspects_[start + i] = Aspect::COLOR_BIT;
	}
}

void Context::BindSamplerStates(int start, int count, SamplerState **states) {
	for (int i = 0; i < count && start + i < MAX_TEXTURE_SLOTS; ++i) {
		samplerRefs_[start + i] = static_cast<Sampler *>(states[i]);
		samplers_[start + i] = samplerRefs_[start + i].ptr;
	}
}

bool Context::Apply() {
	if (!pipeline_) {
		return false;
	}
	auto cmd = Commands();
	if (stateDirty_) {
		if (device_->shaderCodeDirty.load(std::memory_order_relaxed) &&
			device_->shaderCodeDirty.exchange(false, std::memory_order_acq_rel)) {
			cmd.barrier(DkBarrier_None, DkInvalidateFlags_Shader);
		}
		DkShader const *shaders[2]{};
		uint32_t clipDistanceMode = 0;
		for (auto &shader : pipeline_->shaders) {
			shaders[shader->stage == ShaderStage::Vertex ? 0 : 1] = &shader->shader;
			if (shader->stage == ShaderStage::Vertex) {
				clipDistanceMode = shader->clipDistanceMode;
			}
		}
		cmd.bindShaders(DkStageFlag_GraphicsMask, {shaders[0], shaders[1]});
		// CLIP_DISTANCE_MODE (3D method 0x650, nouveau's NVC0_3D_CLIP_DISTANCE_MODE), which deko3d has no state for:
		// an increasing-method header with one data word, on the 3D subchannel (0).
		const uint32_t clipMode[2] = {(1u << 29) | (1u << 16) | 0x650, clipDistanceMode};
		dkCmdBufReplayCmds(cmd, clipMode, 2);
		// All native framebuffers are single-sample. Bind the full draw state,
		// including stages and sample settings unused by PPSSPP.
		cmd.bindMultisampleState(dk::MultisampleState{});
		// Maxwell sample masks use only the low 16 bits, including at 1x.
		cmd.setSampleMask(0xFFFF);
		cmd.setPrimitiveRestart(false, 0xFFFF);
		const auto &raster = pipeline_->raster->desc;
		dk::RasterizerState rasterState;
		const CullMode cull = overrideRaster_ ? cullOverride_ : raster.cull;
		rasterState
			.setCullMode(cull == CullMode::NONE	   ? DkFace_None
						 : cull == CullMode::FRONT ? DkFace_Front
						 : cull == CullMode::BACK  ? DkFace_Back
												   : DkFace_FrontAndBack)
			.setFrontFace(raster.frontFace == Facing::CCW ? DkFrontFace_CCW : DkFrontFace_CW)
			.setDepthClampEnable(depthClamp_)
			.setProvokingVertex(DkProvokingVertex_Last);
		cmd.bindRasterizerState(rasterState);
		const auto &blend = pipeline_->blend->desc;
		dk::ColorState color;
		color.setBlendEnableMask(blend.enabled ? 1 : 0);
		static const DkLogicOp logic[] = {DkLogicOp_Clear,		DkLogicOp_Set,		   DkLogicOp_Copy,		DkLogicOp_CopyInverted,
										  DkLogicOp_NoOp,		DkLogicOp_Invert,	   DkLogicOp_And,		DkLogicOp_Nand,
										  DkLogicOp_Or,			DkLogicOp_Nor,		   DkLogicOp_Xor,		DkLogicOp_Equivalent,
										  DkLogicOp_AndReverse, DkLogicOp_AndInverted, DkLogicOp_OrReverse, DkLogicOp_OrInverted};
		color.logicOp = blend.logicEnabled ? logic[(unsigned)blend.logicOp] : DkLogicOp_Copy;
		cmd.bindColorState(color);
		dk::ColorWriteState write;
		write.setMask(0, blend.colorMask);
		cmd.bindColorWriteState(write);
		dk::BlendState blending;
		blending.setOps((DkBlendOp)((int)blend.eqCol + 1), (DkBlendOp)((int)blend.eqAlpha + 1));
		blending.setFactors(Factor(blend.srcCol), Factor(blend.dstCol), Factor(blend.srcAlpha), Factor(blend.dstAlpha));
		cmd.bindBlendStates(0, blending);
		const auto &depth = pipeline_->depth->desc;
		dk::DepthStencilState stencil;
		stencil.setDepthTestEnable(depth.depthTestEnabled)
			.setDepthWriteEnable(depth.depthWriteEnabled)
			.setDepthCompareOp((DkCompareOp)((int)depth.depthCompare + 1));
		stencil.setStencilTestEnable(depth.stencilEnabled);
		stencil.setStencilFrontCompareOp((DkCompareOp)((int)depth.stencil.compareOp + 1))
			.setStencilBackCompareOp((DkCompareOp)((int)depth.stencil.compareOp + 1));
		stencil.setStencilFrontFailOp((DkStencilOp)((int)depth.stencil.failOp + 1))
			.setStencilBackFailOp((DkStencilOp)((int)depth.stencil.failOp + 1));
		stencil.setStencilFrontPassOp((DkStencilOp)((int)depth.stencil.passOp + 1))
			.setStencilBackPassOp((DkStencilOp)((int)depth.stencil.passOp + 1));
		stencil.setStencilFrontDepthFailOp((DkStencilOp)((int)depth.stencil.depthFailOp + 1))
			.setStencilBackDepthFailOp((DkStencilOp)((int)depth.stencil.depthFailOp + 1));
		cmd.bindDepthStencilState(stencil);
		std::array<DkVtxAttribState, 16> attributes{};
		// Fixed attributes still require a valid format, even when unused.
		for (auto &attribute : attributes) {
			attribute.isFixed = true;
			attribute.size = DkVtxAttribSize_1x32;
			attribute.type = DkVtxAttribType_Float;
		}
		for (const auto &attribute : pipeline_->layout->desc.attributes) {
			if ((unsigned)attribute.location >= attributes.size()) {
				return false;
			}
			DkVtxAttribState &out = attributes[attribute.location];
			out.isFixed = false;
			out.offset = attribute.offset;
			out.type = DkVtxAttribType_Float;
			switch (attribute.format) {
			case DataFormat::R32_FLOAT:
				out.size = DkVtxAttribSize_1x32;
				break;
			case DataFormat::R32G32_FLOAT:
				out.size = DkVtxAttribSize_2x32;
				break;
			case DataFormat::R32G32B32_FLOAT:
				out.size = DkVtxAttribSize_3x32;
				break;
			case DataFormat::R32G32B32A32_FLOAT:
				out.size = DkVtxAttribSize_4x32;
				break;
			case DataFormat::R8G8B8A8_UNORM:
				out.size = DkVtxAttribSize_4x8;
				out.type = DkVtxAttribType_Unorm;
				break;
			case DataFormat::R8G8B8A8_SNORM:
				out.size = DkVtxAttribSize_4x8;
				out.type = DkVtxAttribType_Snorm;
				break;
			case DataFormat::R16G16_UNORM:
				out.size = DkVtxAttribSize_2x16;
				out.type = DkVtxAttribType_Unorm;
				break;
			case DataFormat::R16G16B16A16_UNORM:
				out.size = DkVtxAttribSize_4x16;
				out.type = DkVtxAttribType_Unorm;
				break;
			case DataFormat::R16G16B16A16_SNORM:
				out.size = DkVtxAttribSize_4x16;
				out.type = DkVtxAttribType_Snorm;
				break;
			default:
				return false;
			}
		}
		cmd.bindVtxAttribState(attributes);
		DkVtxBufferState buffer{(uint32_t)pipeline_->layout->desc.stride, 0};
		cmd.bindVtxBufferState(buffer);
		stateDirty_ = false;
	}
	Frame &frame = Current();
	if (frame.imageCount + MAX_TEXTURE_SLOTS >= IMAGE_DESCRIPTORS || frame.samplerCount + MAX_TEXTURE_SLOTS >= SAMPLER_DESCRIPTORS) {
		Submit(true);
		frame.imageCount = frame.samplerCount = 0;
		frame.imageCache.clear();
		frame.samplerCache.clear();
		return Apply();
	}
	auto *images = (DkImageDescriptor *)frame.descriptors.getCpuAddr();
	auto *samplers = (DkSamplerDescriptor *)(images + IMAGE_DESCRIPTORS);
	std::array<DkResHandle, MAX_TEXTURE_SLOTS> handles{};
	bool changed = false;
	for (unsigned i = 0; i < MAX_TEXTURE_SLOTS; ++i) {
		if (!(pipeline_->textures & (1u << i))) {
			continue;
		}
		Image *image = textures_[i] ? textures_[i] : nullTexture_.ptr;
		Sampler *sampler = samplers_[i] ? samplers_[i] : defaultSampler_.ptr;
		if (!image || !sampler) {
			return false;
		}
		const bool array = (pipeline_->arrays & (1u << i)) != 0;
		const bool stencilView = textureAspects_[i] == Aspect::STENCIL_BIT;
		const uint64_t key = image->identity * 4 + array + (stencilView ? 2 : 0);
		auto found = frame.imageCache.find(key);
		uint32_t imageIndex;
		if (found == frame.imageCache.end()) {
			imageIndex = frame.imageCount++;
			DkImageView view = image->view;
			if (stencilView) {
				view.dsSource = DkDsSource_Stencil;
			}
			if (array) {
				view.type = DkImageType_2DArray;
				view.layerCount = 1;
			}
			dkImageDescriptorInitialize(&images[imageIndex], &view, false, false);
			frame.imageCache.emplace(key, imageIndex);
			changed = true;
		} else {
			imageIndex = found->second;
		}
		auto sample = frame.samplerCache.find(sampler->identity);
		uint32_t samplerIndex;
		if (sample == frame.samplerCache.end()) {
			samplerIndex = frame.samplerCount++;
			samplers[samplerIndex] = sampler->descriptor;
			frame.samplerCache.emplace(sampler->identity, samplerIndex);
			changed = true;
		} else {
			samplerIndex = sample->second;
		}
		handles[i] = dkMakeTextureHandle(imageIndex, samplerIndex);
	}
	if (changed) {
		cmd.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
	}
	resourceBindings_.Apply(cmd, handles, uniforms_);
	return true;
}

void Context::SetScissorRect(int x, int y, int w, int h) {
	DkScissor scissor{(uint32_t)std::max(0, x), (uint32_t)std::max(0, y), (uint32_t)std::max(0, w), (uint32_t)std::max(0, h)};
	scissor_ = scissor;
	Commands().setScissors(0, scissor);
}

void Context::SetViewport(const Viewport &vp) {
	DkViewport viewport{vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth};
	viewport_ = viewport;
	Commands().setViewports(0, viewport);
}

void Context::SetBlendFactor(float color[4]) {
	std::copy(color, color + 4, blendConstant_.begin());
	Commands().setBlendConst(color[0], color[1], color[2], color[3]);
}
void Context::SetStencilParams(uint8_t ref, uint8_t write, uint8_t compare) {
	stencilParams_ = {ref, write, compare};
	Commands().setStencil(DkFace_FrontAndBack, write, ref, compare);
}

void Context::RestoreDynamicState() {
	if (targetBound_) {
		Image *color = Color(target_), *depth = DepthImage(target_);
		if (color) {
			Current().commands.bindRenderTargets(&color->view, depth ? &depth->view : nullptr);
		}
	}
	Current().commands.setViewports(0, viewport_);
	Current().commands.setScissors(0, scissor_);
	Current().commands.setBlendConst(blendConstant_[0], blendConstant_[1], blendConstant_[2], blendConstant_[3]);
	Current().commands.setStencil(DkFace_FrontAndBack, stencilParams_[1], stencilParams_[0], stencilParams_[2]);
}

bool Context::DispatchScale(ShaderModule *module, Deko3DUpload input, Texture *sourceTexture, Texture *targetTexture, int level,
							const void *params, size_t paramSize, unsigned groupsX, unsigned groupsY, const void *constants,
							size_t constantSize) {
	auto *shader = static_cast<Shader *>(module);
	auto *source = static_cast<Image *>(sourceTexture);
	auto *target = static_cast<Image *>(targetTexture);
	if (!shader || shader->stage != ShaderStage::Compute || !target || level < 0 || level >= target->levels || !paramSize ||
		paramSize > 256 || !groupsX || !groupsY) {
		return false;
	}
	Commands();
	Frame &frame = Current();
	if (frame.imageCount + 2 >= IMAGE_DESCRIPTORS || frame.samplerCount + 1 >= SAMPLER_DESCRIPTORS) {
		Submit(true);
		frame.imageCount = frame.samplerCount = 0;
		frame.imageCache.clear();
		frame.samplerCache.clear();
	}
	Upload uniforms = Allocate(256);
	if (!uniforms.cpu) {
		return false;
	}
	memset(uniforms.cpu, 0, 256);
	memcpy(uniforms.cpu, params, paramSize);
	auto cmd = Commands();
	cmd.barrier(DkBarrier_Primitives, DkInvalidateFlags_Image | DkInvalidateFlags_Shader);
	resourceBindings_.Invalidate();
	cmd.bindShaders(DkStageFlag_Compute, &shader->shader);
	cmd.bindUniformBuffer(DkStage_Compute, 5, uniforms.gpu.addr, 256);
	if (input.buffer) {
		cmd.bindStorageBuffer(DkStage_Compute, 1, input.buffer, input.size);
	}
	if (constants && constantSize) {
		Upload data = Push(constants, (uint32_t)constantSize, 256);
		if (!data.cpu) {
			return false;
		}
		cmd.bindStorageBuffer(DkStage_Compute, 4, data.gpu.addr, data.gpu.size);
	}
	auto *descriptors = (DkImageDescriptor *)frame.descriptors.getCpuAddr();
	DkImageView output = target->view;
	output.mipLevelOffset = level;
	output.mipLevelCount = 1;
	const uint32_t outputIndex = frame.imageCount++;
	dkImageDescriptorInitialize(&descriptors[outputIndex], &output, true, false);
	DkResHandle outputHandle = dkMakeImageHandle(outputIndex);
	cmd.bindImages(DkStage_Compute, 0, outputHandle);
	if (source) {
		const uint32_t imageIndex = frame.imageCount++;
		dkImageDescriptorInitialize(&descriptors[imageIndex], &source->view, true, false);
		DkResHandle handle = dkMakeImageHandle(imageIndex);
		cmd.bindImages(DkStage_Compute, 3, handle);
	}
	cmd.barrier(DkBarrier_None, DkInvalidateFlags_Descriptors);
	cmd.dispatchCompute(groupsX, groupsY, 1);
	cmd.barrier(DkBarrier_Primitives, DkInvalidateFlags_Image | DkInvalidateFlags_Shader);
	stateDirty_ = true;
	return true;
}

void Context::DrawNative(Primitive primitive, uint64_t vertices, uint32_t size, int count, const uint16_t *indices) {
	if (!vertices || !size || count <= 0 || !Apply()) {
		return;
	}
	static constexpr DkPrimitive primitives[] = {DkPrimitive_Points,	DkPrimitive_Lines,		   DkPrimitive_LineStrip,
												 DkPrimitive_Triangles, DkPrimitive_TriangleStrip, DkPrimitive_TriangleFan};
	if ((unsigned)primitive >= std::size(primitives)) {
		return;
	}
	Commands().bindVtxBuffer(0, vertices, size);
	if (indices) {
		Upload upload = Push(indices, count * sizeof(uint16_t), 4);
		if (!upload.cpu) {
			return;
		}
		Commands().bindIdxBuffer(DkIdxFormat_Uint16, upload.gpu.addr);
		Commands().drawIndexed(primitives[(unsigned)primitive], count, 1, 0, 0, 0);
	} else {
		Commands().draw(primitives[(unsigned)primitive], count, 1, 0, 0);
	}
}

bool Context::ReadTexture(Texture *texture, int level, void *pixels, int stride) {
	auto *image = static_cast<Image *>(texture);
	const int bpp = (int)DataFormatSizeInBytes(image->Format());
	if (level < 0 || level >= image->levels || !bpp || image->Depth() != 1) {
		return false;
	}
	const int w = std::max(1, image->Width() >> level), h = std::max(1, image->Height() >> level);
	if (stride < w * bpp) {
		return false;
	}
	auto memory = device_->Allocate(w * h * bpp, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "texture readback");
	if (!memory) {
		return false;
	}
	DkImageView view = image->view;
	view.mipLevelOffset = level;
	view.mipLevelCount = 1;
	DkCopyBuf buffer{memory.getGpuAddr(), 0, 0};
	DkImageRect region{0, 0, 0, (uint32_t)w, (uint32_t)h, 1};
	Commands().barrier(DkBarrier_Primitives, DkInvalidateFlags_Image);
	Commands().copyImageToBuffer(view, region, buffer);
	Submit(true);
	for (int row = 0; row < h; ++row) {
		memcpy((uint8_t *)pixels + row * stride, (uint8_t *)memory.getCpuAddr() + row * w * bpp, w * bpp);
	}
	return true;
}

void Context::DrawUP(const void *vertices, int count) {
	if (!Apply()) {
		return;
	}
	Upload upload = Push(vertices, count * pipeline_->layout->desc.stride, 16);
	if (!upload.cpu) {
		return;
	}
	Commands().bindVtxBuffer(0, upload.gpu.addr, upload.gpu.size);
	Commands().draw(pipeline_->primitive, count, 1, 0, 0);
}

void Context::DrawIndexedUP(const void *vertices, int vertexCount, const void *indices, int indexCount) {
	if (!Apply()) {
		return;
	}
	Upload v = Push(vertices, vertexCount * pipeline_->layout->desc.stride, 16);
	Upload i = Push(indices, indexCount * sizeof(uint16_t), 4);
	if (!v.cpu || !i.cpu) {
		return;
	}
	Commands().bindVtxBuffer(0, v.gpu.addr, v.gpu.size);
	Commands().bindIdxBuffer(DkIdxFormat_Uint16, i.gpu.addr);
	Commands().drawIndexed(pipeline_->primitive, indexCount, 1, 0, 0, 0);
}

void Context::Draw(int count, int offset) {
	if (!vertex_ || !pipeline_) {
		return;
	}
	const int stride = pipeline_->layout->desc.stride;
	const size_t begin = vertexOffset_ + (size_t)offset * stride;
	if (begin > vertex_->bytes.size() || (size_t)count * stride > vertex_->bytes.size() - begin) {
		return;
	}
	DrawUP(vertex_->bytes.data() + begin, count);
}

void Context::DrawIndexed(int count, int offset) {
	if (!vertex_ || !index_ || !pipeline_) {
		return;
	}
	const size_t indexBegin = indexOffset_ + (size_t)offset * sizeof(uint16_t);
	if (indexBegin > index_->bytes.size() || (size_t)count * 2 > index_->bytes.size() - indexBegin) {
		return;
	}
	const int stride = pipeline_->layout->desc.stride;
	if (vertexOffset_ < 0 || (size_t)vertexOffset_ >= vertex_->bytes.size() || stride <= 0) {
		return;
	}
	DrawIndexedUP(vertex_->bytes.data() + vertexOffset_, (int)(vertex_->bytes.size() - vertexOffset_) / stride,
				  index_->bytes.data() + indexBegin, count);
}

void Context::DrawIndexedClippedBatchUP(const void *vertices, int vertexCount, const void *indices, int indexCount,
										Slice<ClippedDraw> draws, const void *uniforms, size_t size) {
	for (const ClippedDraw &draw : draws) {
		BindPipeline(draw.pipeline);
		BindSamplerStates(0, 1, const_cast<SamplerState **>(&draw.samplerState));
		if (draw.bindFramebufferAsTex) {
			BindFramebufferAsTexture(draw.bindFramebufferAsTex, 0, draw.aspect, 0);
		} else if (draw.bindNativeTexture) {
			BindNativeTexture(0, draw.bindNativeTexture);
		} else {
			BindTexture(0, draw.bindTexture);
		}
		UpdateDynamicUniformBuffer(uniforms, size);
		SetScissorRect(draw.clipx, draw.clipy, draw.clipw, draw.cliph);
		DrawIndexedUP(vertices, vertexCount, (const uint16_t *)indices + draw.indexOffset, draw.indexCount);
	}
}

bool Context::Swapchain(std::string *error) {
	uint32_t width = 0, height = 0;
	if (R_FAILED(nwindowGetDimensions((NWindow *)window_, &width, &height)) || !width || !height) {
		*error = "Invalid deko3d display dimensions";
		return false;
	}
	dk::ImageLayout layout;
	dk::ImageLayoutMaker{device_->device}
		.setFormat(DkImageFormat_RGBA8_Unorm)
		.setDimensions(width, height)
		.setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression)
		.initialize(layout);
	std::array<DkImage const *, 3> images{};
	for (unsigned i = 0; i < images.size(); ++i) {
		auto *image = new Image();
		image->owner = device_;
		image->identity = device_->nextImage++;
		TextureDesc desc{};
		desc.width = width;
		desc.height = height;
		desc.depth = desc.mipLevels = 1;
		desc.format = DataFormat::R8G8B8A8_UNORM;
		image->Dimensions(desc);
		image->memory = device_->Allocate((uint32_t)layout.getSize(), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "swapchain image");
		if (!image->memory) {
			image->Release();
			*error = "Could not allocate deko3d backbuffers";
			return false;
		}
		image->image.initialize(layout, image->memory, 0);
		dkImageViewDefaults(&image->view, &image->image);
		backbuffers_[i].reset(image);
		images[i] = &image->image;
	}
	swapchain_ = dk::SwapchainMaker{device_->device, window_, images}.create();
	if (!swapchain_) {
		*error = "Could not create the deko3d swapchain";
		return false;
	}
	TextureDesc depth{};
	depth.type = TextureType::LINEAR2D;
	depth.width = width;
	depth.height = height;
	depth.depth = depth.mipLevels = 1;
	depth.format = DataFormat::D24_S8;
	backDepth_.reset(static_cast<Image *>(CreateImage(depth, true)));
	if (!backDepth_) {
		*error = "Could not allocate the deko3d depth buffer";
		return false;
	}
	primarySwapchain_->SetSize(width, height);
	SetTargetSize(width, height);
	return true;
}

void Context::Resize() {
	uint32_t width = 0, height = 0;
	if (swapchain_ && backbuffers_[0] && R_SUCCEEDED(nwindowGetDimensions((NWindow *)window_, &width, &height)) &&
		width == (uint32_t)backbuffers_[0]->Width() && height == (uint32_t)backbuffers_[0]->Height()) {
		return;
	}
	StopThreads();
	if (ReportQueueFault("resize")) {
		return;
	}
	frameGenerator_.reset();
	generatedDisplay_.reset(nullptr);
	frameGenerationFailed_ = false;
	swapchain_ = nullptr;
	backDepth_.reset(nullptr);
	for (auto &image : backbuffers_) {
		image.reset(nullptr);
	}
	StartThreads();
	acquired_ = -1;
	std::string error;
	if (!Swapchain(&error)) {
		ERROR_LOG(Log::G3D, "%s", error.c_str());
	}
	stateDirty_ = true;
}

Image *Context::Color(Framebuffer *framebuffer) {
	framebuffer = PrimaryToNull(framebuffer);
	if (framebuffer) {
		return static_cast<FramebufferDeko *>(framebuffer)->color.ptr;
	}
	Commands();
	displayUsed_ = true;
	if (generatedDisplay_) {
		return generatedDisplay_.ptr;
	}
	if (acquired_ < 0) {
		if (ReportQueueFault("acquire")) {
			return nullptr;
		}
		acquired_ = device_->queue.acquireImage(swapchain_);
	}
	return acquired_ >= 0 && acquired_ < (int)backbuffers_.size() ? backbuffers_[acquired_].ptr : nullptr;
}

Image *Context::DepthImage(Framebuffer *framebuffer) {
	framebuffer = PrimaryToNull(framebuffer);
	return framebuffer ? static_cast<FramebufferDeko *>(framebuffer)->depth.ptr : backDepth_.ptr;
}

void Context::GetFramebufferDimensions(Framebuffer *fbo, int *w, int *h) {
	fbo = fbo ? fbo : primarySwapchain_;
	*w = fbo->Width();
	*h = fbo->Height();
}

void Context::BindFramebufferAsRenderTarget(Framebuffer *framebuffer, const RenderPassInfo &rp, const char *tag) {
	Image *color = Color(framebuffer), *depth = DepthImage(framebuffer);
	if (!color) {
		return;
	}
	const DkImageView *view = &color->view;
	Commands().barrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
	Commands().bindRenderTargets(view, depth ? &depth->view : nullptr);
	target_ = framebuffer;
	targetBound_ = true;
	targetRef_ = static_cast<FramebufferDeko *>(PrimaryToNull(framebuffer));
	if (rp.color == RPAction::CLEAR || rp.depth == RPAction::CLEAR || rp.stencil == RPAction::CLEAR) {
		SetScissorRect(0, 0, color->Width(), color->Height());
		Aspect aspects = Aspect::NO_BIT;
		if (rp.color == RPAction::CLEAR) {
			aspects |= Aspect::COLOR_BIT;
		}
		if (rp.depth == RPAction::CLEAR) {
			aspects |= Aspect::DEPTH_BIT;
		}
		if (rp.stencil == RPAction::CLEAR) {
			aspects |= Aspect::STENCIL_BIT;
		}
		Clear(aspects, rp.clearColor, rp.clearDepth, rp.clearStencil);
	}
	if (invalidated_) {
		invalidated_(InvalidationCallbackFlags::RENDER_PASS_STATE);
	}
}

void Context::BindFramebufferAsTexture(Framebuffer *fbo, int binding, Aspect aspect, int layer) {
	if ((unsigned)binding >= MAX_TEXTURE_SLOTS) {
		return;
	}
	textures_[binding] = (aspect & Aspect::COLOR_BIT) ? Color(fbo) : DepthImage(fbo);
	textureRefs_[binding] = textures_[binding];
	textureAspects_[binding] = aspect;
	Commands().barrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
}

void Context::Clear(Aspect aspects, uint32_t color, float depth, int stencil) {
	// deko3d clears stay inside the scissor, but thin3d clears the whole target (as Vulkan and D3D11 do).
	// The current scissor can still be a smaller target's, like the backbuffer's after the UI.
	Image *target = Color(target_);
	if (target) {
		Commands().setScissors(0, DkScissor{0, 0, (uint32_t)target->Width(), (uint32_t)target->Height()});
	}
	if (aspects & Aspect::COLOR_BIT) {
		const float rgba[] = {(color & 255) / 255.f, ((color >> 8) & 255) / 255.f, ((color >> 16) & 255) / 255.f, (color >> 24) / 255.f};
		Commands().clearColor(0, DkColorMask_RGBA, rgba[0], rgba[1], rgba[2], rgba[3]);
	}
	if ((aspects & (Aspect::DEPTH_BIT | Aspect::STENCIL_BIT)) && DepthImage(target_)) {
		Commands().clearDepthStencil((bool)(aspects & Aspect::DEPTH_BIT), depth, (aspects & Aspect::STENCIL_BIT) ? 255 : 0, stencil);
	}
	if (target) {
		Commands().setScissors(0, scissor_);
	}
}

void Context::CopyFramebufferImage(Framebuffer *src, int level, int x, int y, int z, Framebuffer *dst, int dstLevel, int dx, int dy, int dz,
								   int w, int h, int depth, Aspect aspects, const char *tag) {
	if (w <= 0 || h <= 0 || depth != 1 || x < 0 || y < 0 || dx < 0 || dy < 0) {
		return;
	}
	const bool color = aspects & Aspect::COLOR_BIT;
	const bool zPlane = aspects & Aspect::DEPTH_BIT, sPlane = aspects & Aspect::STENCIL_BIT;
	for (int plane = 0; plane < 2; ++plane) {
		if ((!plane && !color) || (plane && !zPlane && !sPlane)) {
			continue;
		}
		Image *source = plane ? DepthImage(src) : Color(src);
		Image *target = plane ? DepthImage(dst) : Color(dst);
		if (!source || !target) {
			continue;
		}
		const int width = std::min(w, std::min(source->Width() - x, target->Width() - dx));
		const int height = std::min(h, std::min(source->Height() - y, target->Height() - dy));
		if (width <= 0 || height <= 0) {
			continue;
		}
		DkImageView a = source->view, b = target->view;
		a.mipLevelOffset = level;
		a.mipLevelCount = 1;
		b.mipLevelOffset = dstLevel;
		b.mipLevelCount = 1;
		DkImageRect from{(uint32_t)x, (uint32_t)y, (uint32_t)z, (uint32_t)width, (uint32_t)height, 1};
		DkImageRect to{(uint32_t)dx, (uint32_t)dy, (uint32_t)dz, (uint32_t)width, (uint32_t)height, 1};
		Commands().barrier(DkBarrier_Primitives, DkInvalidateFlags_Image);
		if (plane && zPlane != sPlane) {
			// The copy engine transfers packed Z24S8. Keep the unrequested plane intact.
			// Ordinary depth blits use FramebufferManagerCommon's shader path instead.
			const uint32_t bytes = width * height * 4;
			auto memory = device_->Allocate(bytes * 2, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "depth/stencil merge");
			if (!memory) {
				return;
			}
			DkCopyBuf sourceBuffer{memory.getGpuAddr(), 0, 0};
			DkCopyBuf targetBuffer{memory.getGpuAddr() + bytes, 0, 0};
			Commands().copyImageToBuffer(a, from, sourceBuffer);
			Commands().copyImageToBuffer(b, to, targetBuffer);
			Submit(true);
			auto *pixels = (uint32_t *)memory.getCpuAddr();
			const uint32_t mask = zPlane ? 0xFFFFFF00 : 0x000000FF;
			for (uint32_t i = 0; i < bytes / 4; ++i) {
				pixels[i] = (pixels[i] & mask) | (pixels[bytes / 4 + i] & ~mask);
			}
			Commands().copyBufferToImage(sourceBuffer, b, to);
			device_->Retire(std::move(memory));
		} else {
			Commands().copyImage(a, from, b, to);
		}
		Commands().barrier(DkBarrier_None, DkInvalidateFlags_Image);
	}
}

bool Context::BlitFramebuffer(Framebuffer *src, int sx1, int sy1, int sx2, int sy2, Framebuffer *dst, int dx1, int dy1, int dx2, int dy2,
							  Aspect aspects, FBBlitFilter filter, const char *tag) {
	if (aspects != Aspect::COLOR_BIT || sx2 == sx1 || sy2 == sy1 || dx2 == dx1 || dy2 == dy1) {
		return false;
	}
	uint32_t flags = filter == FB_BLIT_LINEAR ? DkBlitFlag_FilterLinear : DkBlitFlag_FilterNearest;
	if ((sx2 < sx1) != (dx2 < dx1)) {
		flags |= DkBlitFlag_FlipX;
	}
	if ((sy2 < sy1) != (dy2 < dy1)) {
		flags |= DkBlitFlag_FlipY;
	}
	if (sx2 < sx1) {
		std::swap(sx1, sx2);
	}
	if (sy2 < sy1) {
		std::swap(sy1, sy2);
	}
	if (dx2 < dx1) {
		std::swap(dx1, dx2);
	}
	if (dy2 < dy1) {
		std::swap(dy1, dy2);
	}
	if (sx1 < 0 || sy1 < 0 || dx1 < 0 || dy1 < 0) {
		return false;
	}
	Image *source = Color(src), *target = Color(dst);
	if (!source || !target || sx2 > source->Width() || sy2 > source->Height() || dx2 > target->Width() || dy2 > target->Height()) {
		return false;
	}
	DkImageRect from{(uint32_t)sx1, (uint32_t)sy1, 0, (uint32_t)(sx2 - sx1), (uint32_t)(sy2 - sy1), 1};
	DkImageRect to{(uint32_t)dx1, (uint32_t)dy1, 0, (uint32_t)(dx2 - dx1), (uint32_t)(dy2 - dy1), 1};
	Commands().barrier(DkBarrier_Primitives, DkInvalidateFlags_Image);
	Commands().blitImage(source->view, from, target->view, to, flags);
	Commands().barrier(DkBarrier_None, DkInvalidateFlags_Image);
	return true;
}

bool Context::CopyFramebufferToMemory(Framebuffer *src, Aspect aspect, int x, int y, int w, int h, DataFormat format, void *pixels,
									  int stride, ReadbackMode mode, const char *tag) {
	if (w <= 0 || h <= 0 || stride < w || !pixels || x < 0 || y < 0) {
		return false;
	}
	if (aspect == Aspect::COLOR_BIT) {
		switch (format) {
		case DataFormat::R8G8B8A8_UNORM:
		case DataFormat::R8G8B8_UNORM:
		case DataFormat::R5G6B5_UNORM_PACK16:
		case DataFormat::A1R5G5B5_UNORM_PACK16:
		case DataFormat::A4R4G4B4_UNORM_PACK16:
			break;
		default:
			return false;
		}
	}
	Image *source = aspect == Aspect::COLOR_BIT ? Color(src) : DepthImage(src);
	if (!source || x + w > source->Width() || y + h > source->Height()) {
		return false;
	}
	auto readback = device_->Allocate(w * h * 4, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "framebuffer readback");
	if (!readback) {
		return false;
	}
	DkCopyBuf buffer{readback.getGpuAddr(), 0, 0};
	DkImageRect region{(uint32_t)x, (uint32_t)y, 0, (uint32_t)w, (uint32_t)h, 1};
	Commands().barrier(DkBarrier_Primitives, DkInvalidateFlags_Image);
	Commands().copyImageToBuffer(source->view, region, buffer);
	Submit(true);
	const uint8_t *data = (const uint8_t *)readback.getCpuAddr();
	if (aspect == Aspect::COLOR_BIT) {
		ConvertFromRGBA8888((uint8_t *)pixels, data, stride, w, w, h, format);
	} else if (aspect == Aspect::DEPTH_BIT && (format == DataFormat::D16 || format == DataFormat::D32F)) {
		// Z24S8 stores stencil in the low byte and normalized depth in the upper 24 bits.
		std::vector<uint32_t> packed((size_t)w * h);
		for (size_t i = 0; i < packed.size(); ++i) {
			uint32_t value;
			memcpy(&value, data + i * 4, 4);
			packed[i] = value >> 8;
		}
		if (format == DataFormat::D16) {
			ConvertToD16((uint8_t *)pixels, (uint8_t *)packed.data(), stride, w, w, h, DataFormat::D24_S8);
		} else {
			ConvertToD32F((uint8_t *)pixels, (uint8_t *)packed.data(), stride, w, w, h, DataFormat::D24_S8);
		}
	} else if (aspect == Aspect::STENCIL_BIT && format == DataFormat::S8) {
		for (int row = 0; row < h; ++row) {
			for (int col = 0; col < w; ++col) {
				((uint8_t *)pixels)[row * stride + col] = data[(row * w + col) * 4];
			}
		}
	} else {
		return false;
	}
	return true;
}

std::string Context::GetInfoString(InfoField info) const {
	switch (info) {
	case InfoField::APINAME:
		return "deko3d";
	case InfoField::APIVERSION:
		return "Native Horizon";
	case InfoField::VENDORSTRING:
	case InfoField::VENDOR:
		return "NVIDIA";
	case InfoField::SHADELANGVERSION:
		return "GLSL 450 / UAM";
	case InfoField::DRIVER:
		return "deko3d";
	default:
		return "";
	}
}

uint64_t Context::GetNativeObject(NativeObject object, void *source) {
	switch (object) {
	case NativeObject::DEVICE:
		return (uint64_t)(DkDevice)device_->device;
	case NativeObject::RENDER_MANAGER:
		return (uint64_t)this;
	case NativeObject::TEXTURE_VIEW:
		return (uint64_t)source;
	case NativeObject::BOUND_TEXTURE0_IMAGEVIEW:
		return (uint64_t)textures_[0];
	case NativeObject::BOUND_TEXTURE1_IMAGEVIEW:
		return (uint64_t)textures_[1];
	case NativeObject::NULL_IMAGEVIEW:
	case NativeObject::NULL_IMAGEVIEW_ARRAY:
		return (uint64_t)nullTexture_.ptr;
	default:
		return 0;
	}
}

void Context::HandleEvent(Event event, int w, int h, void *, void *) {
	if (event == Event::GOT_BACKBUFFER) {
		primarySwapchain_->SetSize(w, h);
	}
}

} // namespace

DrawContext *T3DCreateDeko3DContext(void *window, std::string *error) {
	auto context = std::make_unique<Context>();
	if (!context->Init(window, error)) {
		return nullptr;
	}
	return context.release();
}

ShaderModule *Deko3DCreateEmulationShader(DrawContext *draw, ShaderStage stage, const std::string &source, const char *tag) {
	// PSP shaders already use native texture slots 0, 1 and 2, with UBOs at 3/4.
	return static_cast<Context *>(draw)->CreateNativeShader(stage, GLSL_VULKAN, (const uint8_t *)source.data(), source.size(), tag, 0);
}

Deko3DUpload Deko3DAllocate(DrawContext *draw, uint32_t size, uint32_t alignment) {
	return static_cast<Context *>(draw)->Stream(size, alignment);
}
Deko3DUpload Deko3DPush(DrawContext *draw, const void *data, uint32_t size, uint32_t alignment) {
	Deko3DUpload upload = Deko3DAllocate(draw, size, alignment);
	if (upload.ptr) {
		memcpy(upload.ptr, data, size);
	}
	return upload;
}
void Deko3DDraw(DrawContext *draw, Primitive primitive, uint64_t vertices, uint32_t size, int count, const uint16_t *indices) {
	static_cast<Context *>(draw)->DrawNative(primitive, vertices, size, count, indices);
}
bool Deko3DDispatchScale(DrawContext *draw, ShaderModule *shader, Deko3DUpload input, Texture *source, Texture *target, int level,
						 const void *params, size_t paramSize, unsigned groupsX, unsigned groupsY, const void *constants,
						 size_t constantSize) {
	return static_cast<Context *>(draw)->DispatchScale(shader, input, source, target, level, params, paramSize, groupsX, groupsY, constants,
													   constantSize);
}
void Deko3DGenerateMips(DrawContext *draw, Texture *texture, int firstLevel) {
	static_cast<Context *>(draw)->GenerateMips(texture, firstLevel);
}
bool Deko3DReadTexture(DrawContext *draw, Texture *texture, int level, void *pixels, int stride) {
	return static_cast<Context *>(draw)->ReadTexture(texture, level, pixels, stride);
}
void Deko3DSetRaster(DrawContext *draw, CullMode cull, bool clamp) { static_cast<Context *>(draw)->SetRaster(cull, clamp); }
void Deko3DBindUniform(DrawContext *draw, unsigned binding, const void *data, size_t size) {
	static_cast<Context *>(draw)->BindUniform(binding, data, size);
}
void Deko3DSetDepthClamp(DrawContext *draw, bool enabled) { static_cast<Context *>(draw)->DepthClamp(enabled); }

SamplerState *Deko3DCreateSampler(DrawContext *draw, const SamplerStateDesc &desc, float minLod, float maxLod, float bias) {
	return static_cast<Context *>(draw)->SamplerWithLod(desc, minLod, maxLod, bias);
}

} // namespace Draw

Deko3DGraphicsContext::~Deko3DGraphicsContext() { ShutdownSurface(); }
bool Deko3DGraphicsContext::InitAPI(void *, std::string *name, std::string *) {
	*name = "Nintendo Switch Tegra X1 (deko3d)";
	return true;
}
bool Deko3DGraphicsContext::InitSurface(WindowSystem, void *, void *, std::string *error) {
	draw_ = Draw::T3DCreateDeko3DContext(nwindowGetDefault(), error);
	if (!draw_) {
		return false;
	}
	if (!draw_->CreatePresets()) {
		*error = "Failed to compile deko3d UI shaders";
		ShutdownSurface();
		return false;
	}
	SetGPUBackend(GPUBackend::DEKO3D, "NVIDIA Tegra X1");
	return true;
}
void Deko3DGraphicsContext::ShutdownSurface() {
	delete draw_;
	draw_ = nullptr;
}
void Deko3DGraphicsContext::ShutdownAPI() { ShutdownSurface(); }
void Deko3DGraphicsContext::Resize() {
	if (draw_) {
		static_cast<Draw::Context *>(draw_)->Resize();
	}
}
