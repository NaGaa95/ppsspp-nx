// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include <unordered_map>
#include "Common/Data/Collections/Hashmaps.h"
#include "Common/GPU/Deko3D/DekoContext.h"
#include "GPU/GPUState.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/Common/GPUStateUtils.h"
#include "GPU/Common/ShaderId.h"

class ShaderManagerDeko3D;
class TextureCacheDeko3D;
class FramebufferManagerDeko3D;

struct Deko3DBlendKey {
	union {
		uint32_t value;
		struct {
			uint32_t blendEnable : 1;
			uint32_t srcColor : 5; // BlendFactor
			uint32_t dstColor : 5;
			uint32_t srcAlpha : 5;
			uint32_t dstAlpha : 5;
			uint32_t eqColor : 3; // BlendEq
			uint32_t eqAlpha : 3;
			uint32_t colorWriteMask : 4; // R=1, G=2, B=4, A=8
		};
	};
};

struct Deko3DDepthStencilKey {
	union {
		uint64_t value;
		struct {
			uint64_t depthTestEnable : 1;
			uint64_t depthWriteEnable : 1;
			uint64_t depthCompare : 3; // GEComparison
			uint64_t stencilTestEnable : 1;
			uint64_t stencilCompare : 3; // GEComparison
			uint64_t stencilPass : 3;	 // GEStencilOp
			uint64_t stencilFail : 3;
			uint64_t stencilDepthFail : 3;
			uint64_t writeMask : 8;
			uint64_t compareMask : 8;
		};
	};
};

struct Deko3DPipelineKey {
	VShaderID vsid;
	FShaderID fsid;
	uint64_t depth;
	uint32_t vtxFmtId;
	uint32_t blend;
	uint32_t logic;
	uint32_t pad = 0;
};

struct Deko3DDynamicState {
	Draw::Viewport viewport{};
	int scissor[4]{};
	bool useBlendColor = false;
	float blendColor[4]{};
	uint8_t stencilRef = 0;
	Draw::CullMode cull = Draw::CullMode::NONE;
	bool depthClamp = false;
};

class DrawEngineDeko3D : public DrawEngineCommon {
public:
	explicit DrawEngineDeko3D(Draw::DrawContext *draw);
	~DrawEngineDeko3D();
	void SetShaderManager(ShaderManagerDeko3D *manager) { shaderManager_ = manager; }
	void SetTextureCache(TextureCacheDeko3D *cache) { textureCache_ = cache; }
	void SetFramebufferManager(FramebufferManagerDeko3D *manager) { framebufferManager_ = manager; }
	void DeviceLost() override;
	void DeviceRestore(Draw::DrawContext *draw) override;
	void BeginFrame() override;
	void Flush() override;
	void FinishDeferred() { Flush(); }
	void ClearPipelines();
	bool SavePipelines(FILE *f);
	static uint32_t PipelineRecordSize();
	bool LoadPipelines(FILE *f, int count, bool waitForCompiles);
	int GetNumPipelines() const { return (int)pipelines_.size(); }
	void SetDepalTexture(Draw::Texture *texture, bool smooth) {
		if (boundDepal_ != texture) {
			boundDepal_ = texture;
			boundDepalSmoothed_ = smooth;
			gstate_c.Dirty(DIRTY_FRAGMENTSHADER_STATE);
		}
	}

private:
	using Pipeline = Draw::Pipeline;
	void InitDeviceObjects();
	void Invalidate(InvalidationCallbackFlags flags);
	void ConvertStateToKeys(int prim);
	void BindShaderBlendTex();
	Pipeline *GetPipeline(const VShaderID &vsid, const FShaderID &fsid, bool hw);
	Pipeline *GetOrCreatePipeline(const Deko3DPipelineKey &key, const DecVtxFormat *format);
	bool ApplyState(Pipeline *pipeline, bool overrideStencil, uint8_t stencil);
	void BindResources(bool pixelMapped);
	void ResetAfterSkippedDraw();
	void DrawPrimitives(Draw::Primitive prim, uint64_t vertices, uint32_t size, int count, const uint16_t *indices);
	Draw::DrawContext *draw_;
	ShaderManagerDeko3D *shaderManager_ = nullptr;
	TextureCacheDeko3D *textureCache_ = nullptr;
	FramebufferManagerDeko3D *framebufferManager_ = nullptr;
	DenseHashMap<Deko3DPipelineKey, Pipeline *> pipelines_;
	Deko3DBlendKey blendKey_{};
	Deko3DDepthStencilKey depthStencilKey_{};
	Deko3DDynamicState dynState_{};
	GEPrimitiveType lastPrim_ = GE_PRIM_INVALID;
	Pipeline *lastPipeline_ = nullptr;
	Draw::Texture *boundSecondary_ = nullptr;
	Draw::Texture *boundDepal_ = nullptr;
	bool boundDepalSmoothed_ = false;
	Draw::SamplerState *samplerNearest_ = nullptr;
	Draw::SamplerState *samplerLinear_ = nullptr;
	FBOTexState fboTexBindState_ = FBO_TEX_NONE;
	bool baseUBO_ = false;
	bool lightUBO_ = false;
	int uboFrame_ = -1;
};
