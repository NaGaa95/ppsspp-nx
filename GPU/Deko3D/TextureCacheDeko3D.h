// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <unordered_map>
#include "Common/GPU/thin3d.h"
#include "GPU/Common/TextureCacheCommon.h"
#include "GPU/Common/TextureShaderCommon.h"
#include "Common/GPU/Deko3D/DekoContext.h"

class FramebufferManagerDeko3D;
class DrawEngineDeko3D;

class TextureCacheDeko3D : public TextureCacheCommon {
public:
	TextureCacheDeko3D(Draw::DrawContext *draw, Draw2D *draw2D);
	~TextureCacheDeko3D();
	void SetFramebufferManager(FramebufferManagerDeko3D *manager);
	void SetDrawEngine(DrawEngineDeko3D *engine) { drawEngine_ = engine; }
	void ForgetLastTexture() override {}
	bool GetCurrentTextureDebug(GPUDebugBuffer &buffer, int level, bool *isFramebuffer) override;
	void DeviceLost() override;
	void DeviceRestore(Draw::DrawContext *draw) override;
	void NotifyConfigChanged() override;
	void *GetNativeTextureView(const TexCacheEntry *entry, bool flat) const override { return entry->texturePtr; }
	Draw::Texture *curTexture = nullptr;
	Draw::SamplerState *curSampler = nullptr;

protected:
	void BindTexture(TexCacheEntry *entry) override;
	void Unbind() override;
	void ReleaseTexture(TexCacheEntry *entry, bool deleteThem) override;
	void BindAsClutTexture(Draw::Texture *texture, bool smooth) override;
	void ApplySamplerByKey(const SamplerCacheKey &key) override;
	void BoundFramebufferTexture() override;
	void BuildTexture(TexCacheEntry *const entry) override;

private:
	void SetBoundTexture(Draw::Texture *texture);
	void SetBoundSampler(Draw::SamplerState *sampler);
	void CompileScalingShaders();
	void ClearScalingShaders();
	bool ScaleTexture(Draw::Deko3DUpload input, Draw::Texture *target, int level, int srcWidth, int srcHeight, int dstWidth, int dstHeight);
	std::string textureShader_;
	std::string scalingConstants_;
	std::vector<Draw::ShaderModule *> scalingShaders_;
	std::vector<TextureScaleScratchDesc> scratchDescs_;
	std::vector<TextureScaleStageDesc> stageDescs_;
	DrawEngineDeko3D *drawEngine_ = nullptr;
	std::unordered_map<uint64_t, Draw::SamplerState *> samplerCache_;
};
