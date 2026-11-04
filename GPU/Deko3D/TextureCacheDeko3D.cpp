// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#include "ppsspp_config.h"
#include <algorithm>
#include <vector>
#include "Common/GPU/Deko3D/DekoContext.h"
#include "Common/Log.h"
#include "Core/Config.h"
#include "Common/File/VFS/VFS.h"
#include "Common/StringUtils.h"
#include "GPU/Common/PostShader.h"
#include "GPU/GPUState.h"
#include "GPU/Common/GPUDebugInterface.h"
#include "GPU/Deko3D/TextureCacheDeko3D.h"
#include "GPU/Deko3D/DrawEngineDeko3D.h"
#include "GPU/Deko3D/FramebufferManagerDeko3D.h"

static Draw::DataFormat PSPTextureFormat(GETextureFormat format, GEPaletteFormat palette) {
	if (format >= GE_TFMT_CLUT4 && format <= GE_TFMT_CLUT32) {
		switch (palette) {
		case GE_CMODE_16BIT_BGR5650:
			return Draw::DataFormat::R5G6B5_UNORM_PACK16;
		case GE_CMODE_16BIT_ABGR5551:
			return Draw::DataFormat::A1R5G5B5_UNORM_PACK16;
		case GE_CMODE_16BIT_ABGR4444:
			return Draw::DataFormat::A4R4G4B4_UNORM_PACK16;
		default:
			return Draw::DataFormat::R8G8B8A8_UNORM;
		}
	}
	switch (format) {
	case GE_TFMT_5650:
		return Draw::DataFormat::R5G6B5_UNORM_PACK16;
	case GE_TFMT_5551:
		return Draw::DataFormat::A1R5G5B5_UNORM_PACK16;
	case GE_TFMT_4444:
		return Draw::DataFormat::A4R4G4B4_UNORM_PACK16;
	default:
		return Draw::DataFormat::R8G8B8A8_UNORM;
	}
}

TextureCacheDeko3D::TextureCacheDeko3D(Draw::DrawContext *draw, Draw2D *draw2D) : TextureCacheCommon(draw, draw2D) {
	CompileScalingShaders();
}

TextureCacheDeko3D::~TextureCacheDeko3D() {
	Clear(true);
	DeviceLost();
}

void TextureCacheDeko3D::SetFramebufferManager(FramebufferManagerDeko3D *manager) { framebufferManager_ = manager; }

void TextureCacheDeko3D::SetBoundTexture(Draw::Texture *texture) {
	if (curTexture == texture) {
		return;
	}
	if (texture) {
		texture->AddRef();
	}
	if (curTexture) {
		curTexture->Release();
	}
	curTexture = texture;
}

void TextureCacheDeko3D::SetBoundSampler(Draw::SamplerState *sampler) {
	if (curSampler == sampler) {
		return;
	}
	if (sampler) {
		sampler->AddRef();
	}
	if (curSampler) {
		curSampler->Release();
	}
	curSampler = sampler;
}

void TextureCacheDeko3D::DeviceLost() {
	TextureCacheCommon::DeviceLost();
	ClearScalingShaders();
	Unbind();
	for (auto &entry : samplerCache_) {
		entry.second->Release();
	}
	samplerCache_.clear();
	draw_ = nullptr;
}

void TextureCacheDeko3D::DeviceRestore(Draw::DrawContext *draw) {
	TextureCacheCommon::DeviceRestore(draw);
	CompileScalingShaders();
}

void TextureCacheDeko3D::ReleaseTexture(TexCacheEntry *entry, bool) {
	if (entry->texturePtr) {
		static_cast<Draw::Texture *>(entry->texturePtr)->Release();
		entry->texturePtr = nullptr;
	}
}

void TextureCacheDeko3D::BindTexture(TexCacheEntry *entry) {
	if (!entry || !entry->texturePtr) {
		Unbind();
		return;
	}
	drawEngine_->SetDepalTexture(nullptr, false);
	SetBoundTexture(static_cast<Draw::Texture *>(entry->texturePtr));
}

void TextureCacheDeko3D::Unbind() {
	SetBoundTexture(nullptr);
	SetBoundSampler(nullptr);
}
void TextureCacheDeko3D::BindAsClutTexture(Draw::Texture *texture, bool smooth) { drawEngine_->SetDepalTexture(texture, smooth); }
void TextureCacheDeko3D::BoundFramebufferTexture() {
	SetBoundTexture((Draw::Texture *)draw_->GetNativeObject(Draw::NativeObject::BOUND_TEXTURE0_IMAGEVIEW));
}

void TextureCacheDeko3D::ApplySamplerByKey(const SamplerCacheKey &key) {
	auto found = samplerCache_.find(key.fullKey);
	if (found != samplerCache_.end()) {
		SetBoundSampler(found->second);
		return;
	}
	Draw::SamplerStateDesc desc{};
	desc.minFilter = key.minFilt ? Draw::TextureFilter::LINEAR : Draw::TextureFilter::NEAREST;
	desc.magFilter = key.magFilt ? Draw::TextureFilter::LINEAR : Draw::TextureFilter::NEAREST;
	desc.mipFilter = key.mipFilt ? Draw::TextureFilter::LINEAR : Draw::TextureFilter::NEAREST;
	desc.wrapU = key.sClamp ? Draw::TextureAddressMode::CLAMP_TO_EDGE : Draw::TextureAddressMode::REPEAT;
	desc.wrapV = key.tClamp ? Draw::TextureAddressMode::CLAMP_TO_EDGE : Draw::TextureAddressMode::REPEAT;
	desc.wrapW = Draw::TextureAddressMode::CLAMP_TO_EDGE;
	desc.maxAniso = key.aniso && key.minFilt && key.magFilt ? (float)std::clamp(1 << key.anisoLevel, 1, 16) : 1.f;
	const float minimum = std::max(0.f, key.minLevel / 256.f);
	const float maximum = key.maxLevel == 9 * 256 ? 1000.f : std::max(minimum, key.maxLevel / 256.f);
	// Keep the PSP's fixed-point bias in the native sampler, as in Vulkan.
	Draw::SamplerState *sampler = Draw::Deko3DCreateSampler(draw_, desc, minimum, maximum, key.lodBias / 256.f);
	samplerCache_[key.fullKey] = sampler;
	SetBoundSampler(sampler);
}

void TextureCacheDeko3D::BuildTexture(TexCacheEntry *const entry) {
	BuildTexturePlan plan;
	plan.hardwareScaling = g_Config.bTexHardwareScaling && !scalingShaders_.empty();
	plan.slowScaler = !plan.hardwareScaling;
	if (!PrepareBuildTexture(plan, entry)) {
		ReleaseTexture(entry, true);
		return;
	}
	Draw::DataFormat format = PSPTextureFormat((GETextureFormat)entry->format, gstate.getClutPaletteFormat());
	if (plan.decodeToClut8) {
		format = Draw::DataFormat::R8_UNORM;
	} else if (plan.doReplace) {
		format = plan.replaced->Format();
	} else if (plan.scaleFactor > 1 || plan.saveTexture) {
		format = Draw::DataFormat::R8G8B8A8_UNORM;
	}
	if (g_Config.iTexFiltering == TEX_FILTER_AUTO_MAX_QUALITY && plan.w <= 256 && plan.h <= 256 && plan.depth == 1 && !plan.doReplace) {
		format = Draw::DataFormat::R8G8B8A8_UNORM;
	}
	const int levelsToLoad = plan.depth > 1 ? 1 : std::min(plan.levelsToCreate, plan.levelsToLoad);
	int levels = levelsToLoad;
	if (g_Config.iTexFiltering == TEX_FILTER_AUTO_MAX_QUALITY && plan.w <= 256 && plan.h <= 256 && plan.depth == 1 &&
		format == Draw::DataFormat::R8G8B8A8_UNORM) {
		levels = plan.maxPossibleLevels;
	}
	if (levels < 1 || !(draw_->GetDataFormatSupport(format) & Draw::FMT_TEXTURE)) {
		ERROR_LOG(Log::G3D, "Unsupported deko3d replacement texture format %s", Draw::DataFormatToString(format));
		ReleaseTexture(entry, true);
		return;
	}
	Draw::TextureDesc desc{};
	desc.type = plan.depth > 1 ? Draw::TextureType::LINEAR3D : Draw::TextureType::LINEAR2D;
	desc.format = format;
	desc.depth = plan.depth;
	desc.mipLevels = levels;
	desc.tag = "PSP texture";
	plan.GetMipSize(0, &desc.width, &desc.height);
	const bool compute = plan.hardwareScaling && plan.scaleFactor > 1 && format == Draw::DataFormat::R8G8B8A8_UNORM && plan.depth == 1 &&
						 !plan.doReplace && !plan.saveTexture;
	if (compute) {
		Draw::Texture *texture = draw_->CreateTexture(desc);
		bool ok = texture != nullptr;
		for (int level = 0; ok && level < levelsToLoad; ++level) {
			const int srcLevel = level ? level : plan.baseLevelSrc;
			const int srcWidth = gstate.getTextureWidth(srcLevel), srcHeight = gstate.getTextureHeight(srcLevel);
			Draw::Deko3DUpload upload = Draw::Deko3DAllocate(draw_, srcWidth * srcHeight * 4, 16);
			if (!upload.ptr) {
				ok = false;
				break;
			}
			BuildTexturePlan unscaled = plan;
			unscaled.scaleFactor = 1;
			LoadTextureLevel(*entry, upload.ptr, upload.size, srcWidth * 4, unscaled, srcLevel, format, TexDecodeFlags::EXPAND32);
			int width, height;
			plan.GetMipSize(level, &width, &height);
			ok = ScaleTexture(upload, texture, level, srcWidth, srcHeight, width, height);
		}
		if (ok) {
			if (levels > levelsToLoad) {
				Draw::Deko3DGenerateMips(draw_, texture, levelsToLoad);
			}
			ReleaseTexture(entry, true);
			entry->texturePtr = texture;
			if (levels == 1) {
				entry->status |= TexStatus::NO_MIPS;
			} else {
				entry->status &= ~TexStatus::NO_MIPS;
			}
			return;
		}
		if (texture) {
			texture->Release();
		}
		// Complete the upload with the CPU scaler if scratch memory cannot be allocated.
		plan.hardwareScaling = false;
		plan.slowScaler = true;
	}
	desc.generateMips = levels > levelsToLoad;
	desc.initData.resize(levelsToLoad, nullptr);
	int level = 0;
	desc.initDataCallback = [&](uint8_t *destination, const uint8_t *, uint32_t width, uint32_t height, uint32_t depth, uint32_t stride,
								uint32_t sliceSize) {
		const int current = level++;
		for (uint32_t slice = 0; slice < depth; ++slice) {
			const int srcLevel = plan.depth > 1 ? (slice ? slice : plan.baseLevelSrc) : (current ? current : plan.baseLevelSrc);
			LoadTextureLevel(*entry, destination + slice * sliceSize, sliceSize, stride, plan, srcLevel, format, TexDecodeFlags{});
		}
		return true;
	};
	Draw::Texture *texture = draw_->CreateTexture(desc);
	if (!texture) {
		ERROR_LOG(Log::G3D, "Could not allocate deko3d texture %dx%d", desc.width, desc.height);
	}
	ReleaseTexture(entry, true);
	entry->texturePtr = texture;
	if (plan.depth > 1) {
		entry->status |= TexStatus::IS_3D;
	}
	if (levels == 1) {
		entry->status |= TexStatus::NO_MIPS;
	} else {
		entry->status &= ~TexStatus::NO_MIPS;
	}
	if (plan.doReplace) {
		entry->SetAlphaStatus(plan.replaced->AlphaStatus());
	}
}

void TextureCacheDeko3D::ClearScalingShaders() {
	for (auto *shader : scalingShaders_) {
		shader->Release();
	}
	scalingShaders_.clear();
	scratchDescs_.clear();
	stageDescs_.clear();
	textureShader_.clear();
	scalingConstants_.clear();
}

void TextureCacheDeko3D::NotifyConfigChanged() {
	TextureCacheCommon::NotifyConfigChanged();
	CompileScalingShaders();
}

void TextureCacheDeko3D::CompileScalingShaders() {
	if (!draw_) {
		return;
	}
	if (!g_Config.bTexHardwareScaling || textureShader_ != g_Config.sTextureShaderName) {
		ClearScalingShaders();
	}
	if (!g_Config.bTexHardwareScaling || !scalingShaders_.empty()) {
		return;
	}
	ReloadAllPostShaderInfo(draw_);
	const TextureShaderInfo *info = GetTextureShaderInfo(g_Config.sTextureShaderName);
	if (!info || info->computeShaderFiles.empty()) {
		return;
	}
	const bool multipass = info->computeShaderFiles.size() > 1;
	if (multipass && !GetTextureScaleMultipassLayout(info->scaleFactor, &scratchDescs_, &stageDescs_)) {
		return;
	}
	for (const Path &file : info->computeShaderFiles) {
		size_t size = 0;
		uint8_t *data = g_VFS.ReadFile(file.c_str(), &size);
		if (!data) {
			ClearScalingShaders();
			return;
		}
		std::string source((const char *)data, size);
		delete[] data;
		if (!multipass) {
			source = StringFromFormat(textureScaleUploadShader, source.c_str());
		}
		auto *shader = draw_->CreateShaderModule(ShaderStage::Compute, GLSL_VULKAN, (const uint8_t *)source.data(), source.size(),
												 info->section.c_str());
		if (!shader) {
			ClearScalingShaders();
			return;
		}
		scalingShaders_.push_back(shader);
	}
	if (!info->constantBuffer.empty()) {
		size_t size = 0;
		uint8_t *data = g_VFS.ReadFile(info->constantBuffer.c_str(), &size);
		if (!data || size > 16 * 1024 * 1024) {
			delete[] data;
			ClearScalingShaders();
			return;
		}
		scalingConstants_.assign((const char *)data, size);
		delete[] data;
	}
	textureShader_ = g_Config.sTextureShaderName;
}

bool TextureCacheDeko3D::ScaleTexture(Draw::Deko3DUpload input, Draw::Texture *target, int level, int sw, int sh, int dw, int dh) {
	if (scalingShaders_.size() == 1) {
		const int32_t params[] = {sw, sh};
		return Draw::Deko3DDispatchScale(draw_, scalingShaders_[0], input, nullptr, target, level, params, sizeof(params), (sw + 7) / 8,
										 (sh + 7) / 8, scalingConstants_.data(), scalingConstants_.size());
	}
	std::vector<Draw::AutoRef<Draw::Texture>> scratch;
	for (const auto &desc : scratchDescs_) {
		Draw::TextureDesc texture{};
		texture.type = Draw::TextureType::LINEAR2D;
		texture.format = Draw::DataFormat::R16G16B16A16_FLOAT;
		texture.width = sw * desc.widthScale;
		texture.height = sh * desc.heightScale;
		texture.depth = texture.mipLevels = 1;
		texture.tag = desc.tag;
		Draw::Texture *image = draw_->CreateTexture(texture);
		if (!image) {
			return false;
		}
		scratch.emplace_back();
		scratch.back().reset(image);
	}
	for (const auto &stage : stageDescs_) {
		if (stage.shaderIndex >= scalingShaders_.size()) {
			return false;
		}
		const int32_t params[] = {sw * stage.srcWidthScale, sh * stage.srcHeightScale,
								  stage.useFinalOutputSize ? dw : sw * stage.dstWidthScale,
								  stage.useFinalOutputSize ? dh : sh * stage.dstHeightScale};
		Draw::Texture *output = stage.outputScratch < 0 ? target : scratch[stage.outputScratch].ptr;
		Draw::Texture *source = stage.inputScratch < 0 ? nullptr : scratch[stage.inputScratch].ptr;
		if (!Draw::Deko3DDispatchScale(draw_, scalingShaders_[stage.shaderIndex], stage.inputScratch < 0 ? input : Draw::Deko3DUpload{},
									   source, output, stage.outputScratch < 0 ? level : 0, params, sizeof(params), (params[2] + 7) / 8,
									   (params[3] + 7) / 8, scalingConstants_.data(), scalingConstants_.size())) {
			return false;
		}
	}
	return true;
}

bool TextureCacheDeko3D::GetCurrentTextureDebug(GPUDebugBuffer &buffer, int level, bool *isFramebuffer) {
	TextureApplyResult result = ApplyTexture(false);
	if (result.framebuffer) {
		*isFramebuffer = true;
		return GetFramebufferTextureDebug(result.framebuffer, result.framebufferTextureChannel, buffer);
	}
	if (!result.texCacheEntry || !result.texCacheEntry->texturePtr) {
		return false;
	}
	auto *texture = static_cast<Draw::Texture *>(result.texCacheEntry->texturePtr);
	GPUDebugBufferFormat format;
	switch (texture->Format()) {
	case Draw::DataFormat::R8_UNORM:
		format = GPU_DBG_FORMAT_8BIT;
		break;
	case Draw::DataFormat::R8G8B8A8_UNORM:
		format = GPU_DBG_FORMAT_8888;
		break;
	case Draw::DataFormat::R5G6B5_UNORM_PACK16:
		format = GPU_DBG_FORMAT_565;
		break;
	case Draw::DataFormat::A1R5G5B5_UNORM_PACK16:
		format = GPU_DBG_FORMAT_5551;
		break;
	case Draw::DataFormat::A4R4G4B4_UNORM_PACK16:
		format = GPU_DBG_FORMAT_4444;
		break;
	default:
		return false;
	}
	const int width = std::max(1, texture->Width() >> level), height = std::max(1, texture->Height() >> level);
	buffer.Allocate(width, height, format);
	*isFramebuffer = false;
	return Draw::Deko3DReadTexture(draw_, texture, level, buffer.GetData(), width * Draw::DataFormatSizeInBytes(texture->Format()));
}
