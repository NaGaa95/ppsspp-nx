// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include "Common/GPU/GraphicsContext.h"
#include "Common/GPU/thin3d.h"

class Path;

namespace Draw {
DrawContext *T3DCreateDeko3DContext(void *nativeWindow, std::string *error);
ShaderModule *Deko3DCreateEmulationShader(DrawContext *draw, ShaderStage stage, const std::string &source, const char *tag);
void Deko3DBindUniform(DrawContext *draw, unsigned binding, const void *data, size_t size);
struct Deko3DUpload {
	uint64_t buffer = 0;
	uint32_t size = 0;
	uint8_t *ptr = nullptr;
};
Deko3DUpload Deko3DAllocate(DrawContext *draw, uint32_t size, uint32_t alignment);
Deko3DUpload Deko3DPush(DrawContext *draw, const void *data, uint32_t size, uint32_t alignment);
void Deko3DDraw(DrawContext *draw, Primitive primitive, uint64_t vertices, uint32_t size, int count, const uint16_t *indices);
bool Deko3DDispatchScale(DrawContext *draw, ShaderModule *shader, Deko3DUpload input, Texture *source, Texture *target, int level,
						 const void *params, size_t paramSize, unsigned groupsX, unsigned groupsY, const void *constants,
						 size_t constantSize);
bool Deko3DReadTexture(DrawContext *draw, Texture *texture, int level, void *pixels, int byteStride);
void Deko3DGenerateMips(DrawContext *draw, Texture *texture, int firstLevel);
void Deko3DSetRaster(DrawContext *draw, CullMode cull, bool depthClamp);
void Deko3DSetDepthClamp(DrawContext *draw, bool enabled);
SamplerState *Deko3DCreateSampler(DrawContext *draw, const SamplerStateDesc &desc, float minLod, float maxLod, float bias);
} // namespace Draw

class Deko3DGraphicsContext : public GraphicsContext {
public:
	~Deko3DGraphicsContext() override;
	bool InitAPI(void *wnd, std::string *deviceName, std::string *error) override;
	bool InitSurface(WindowSystem winsys, void *data1, void *data2, std::string *error) override;
	void ShutdownSurface() override;
	void ShutdownAPI() override;
	void Resize() override;
	Draw::DrawContext *GetDrawContext() override { return draw_; }

private:
	Draw::DrawContext *draw_ = nullptr;
};
