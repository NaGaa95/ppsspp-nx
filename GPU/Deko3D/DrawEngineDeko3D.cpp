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

#include "ppsspp_config.h"
#include <algorithm>
#include <functional>
#include "Common/Log.h"
#include "Common/StringUtils.h"
#include "Common/Profiler/Profiler.h"
#include "Core/Config.h"
#include "GPU/GPUState.h"
#include "GPU/GPUCommon.h"
#include "GPU/ge_constants.h"
#include "GPU/Common/SoftwareTransformCommon.h"
#include "GPU/Common/VertexDecoderCommon.h"
#include "GPU/Deko3D/DrawEngineDeko3D.h"
#include "GPU/Deko3D/ShaderManagerDeko3D.h"
#include "GPU/Deko3D/TextureCacheDeko3D.h"
#include "GPU/Deko3D/FramebufferManagerDeko3D.h"

static constexpr uint32_t SOFTWARE_TRANSFORM_VTXFMT = 0xFFFFFFFF;

static Draw::DataFormat DecFormat(int format) {
	switch (format) {
	case DEC_FLOAT_1:
		return Draw::DataFormat::R32_FLOAT;
	case DEC_FLOAT_2:
		return Draw::DataFormat::R32G32_FLOAT;
	case DEC_FLOAT_3:
		return Draw::DataFormat::R32G32B32_FLOAT;
	case DEC_FLOAT_4:
		return Draw::DataFormat::R32G32B32A32_FLOAT;
	case DEC_S8_3:
		return Draw::DataFormat::R8G8B8A8_SNORM;
	case DEC_S16_3:
		return Draw::DataFormat::R16G16B16A16_SNORM;
	case DEC_U8_1:
	case DEC_U8_2:
	case DEC_U8_3:
	case DEC_U8_4:
		return Draw::DataFormat::R8G8B8A8_UNORM;
	case DEC_U16_1:
	case DEC_U16_2:
		return Draw::DataFormat::R16G16_UNORM;
	case DEC_U16_3:
	case DEC_U16_4:
		return Draw::DataFormat::R16G16B16A16_UNORM;
	default:
		return Draw::DataFormat::UNDEFINED;
	}
}

static Draw::InputLayout *CreateLayout(Draw::DrawContext *draw, const DecVtxFormat *format) {
	Draw::InputLayoutDesc desc{};
	auto attr = [&](PspAttributeLocation location, int type, int offset) {
		if (type) {
			desc.attributes.push_back({(int)location, DecFormat(type), offset});
		}
	};
	if (format) {
		attr(PspAttributeLocation::W1, format->w0fmt, format->w0off);
		attr(PspAttributeLocation::W2, format->w1fmt, format->w1off);
		attr(PspAttributeLocation::TEXCOORD, format->uvfmt, format->uvoff);
		attr(PspAttributeLocation::COLOR0, format->c0fmt, format->c0off);
		attr(PspAttributeLocation::COLOR1, format->c1fmt, format->c1off);
		attr(PspAttributeLocation::NORMAL, format->nrmfmt, format->nrmoff);
		attr(PspAttributeLocation::POSITION, DecVtxFormat::PosFmt(), format->posoff);
		desc.stride = format->stride;
	} else {
		attr(PspAttributeLocation::POSITION, DEC_FLOAT_4, offsetof(TransformedVertex, pos));
		attr(PspAttributeLocation::TEXCOORD, DEC_FLOAT_3, offsetof(TransformedVertex, uv));
		attr(PspAttributeLocation::COLOR0, DEC_U8_4, offsetof(TransformedVertex, color0_32));
		attr(PspAttributeLocation::COLOR1, DEC_U8_4, offsetof(TransformedVertex, color1_32));
		attr(PspAttributeLocation::NORMAL, DEC_FLOAT_1, offsetof(TransformedVertex, fog));
		desc.stride = sizeof(TransformedVertex);
	}
	return draw->CreateInputLayout(desc);
}

DrawEngineDeko3D::DrawEngineDeko3D(Draw::DrawContext *draw) : draw_(draw), pipelines_(256) {
	decOptions_.expand8BitNormalsToFloat = false;
	InitDeviceObjects();
}
DrawEngineDeko3D::~DrawEngineDeko3D() { DeviceLost(); }

void DrawEngineDeko3D::InitDeviceObjects() {
	Draw::SamplerStateDesc desc{};
	desc.wrapU = desc.wrapV = desc.wrapW = Draw::TextureAddressMode::CLAMP_TO_EDGE;
	desc.maxAniso = 1.f;
	samplerNearest_ = draw_->CreateSamplerState(desc);
	desc.minFilter = desc.magFilter = Draw::TextureFilter::LINEAR;
	samplerLinear_ = draw_->CreateSamplerState(desc);
}

void DrawEngineDeko3D::DeviceLost() {
	ClearPipelines();
	if (draw_) {
		draw_->SetInvalidationCallback(InvalidationCallback());
		invalidationCallbackInstalled_ = false;
	}
	if (samplerNearest_) {
		samplerNearest_->Release();
		samplerNearest_ = nullptr;
	}
	if (samplerLinear_) {
		samplerLinear_->Release();
		samplerLinear_ = nullptr;
	}
	boundSecondary_ = boundDepal_ = nullptr;
	baseUBO_ = lightUBO_ = false;
	uboFrame_ = -1;
	draw_ = nullptr;
}

void DrawEngineDeko3D::DeviceRestore(Draw::DrawContext *draw) {
	draw_ = draw;
	InitDeviceObjects();
}

void DrawEngineDeko3D::ClearPipelines() {
	pipelines_.Iterate([](const Deko3DPipelineKey &, Pipeline *pipeline) {
		if (pipeline) {
			pipeline->Release();
		}
	});
	pipelines_.Clear();
	lastPipeline_ = nullptr;
}

void DrawEngineDeko3D::BeginFrame() {
	DrawEngineCommon::BeginFrame();
	if (!invalidationCallbackInstalled_) {
		draw_->SetInvalidationCallback(std::bind(&DrawEngineDeko3D::Invalidate, this, std::placeholders::_1));
		invalidationCallbackInstalled_ = true;
	}
	lastPipeline_ = nullptr;
	boundDepal_ = boundSecondary_ = nullptr;
	baseUBO_ = lightUBO_ = false;
	gstate_c.Dirty(DIRTY_TEXTURE_IMAGE);
	AssertEmpty();
}

void DrawEngineDeko3D::Invalidate(InvalidationCallbackFlags flags) {
	if (flags & InvalidationCallbackFlags::RENDER_PASS_STATE) {
		gstate_c.Dirty(DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_BLEND_STATE | DIRTY_TEXTURE_IMAGE |
					   DIRTY_TEXTURE_PARAMS);
		lastPipeline_ = nullptr;
	}
}

DrawEngineDeko3D::Pipeline *DrawEngineDeko3D::GetPipeline(const VShaderID &vsid, const FShaderID &fsid, bool hw) {
	Deko3DPipelineKey key{};
	key.vsid = vsid;
	key.fsid = fsid;
	key.vtxFmtId = hw ? dec_->decFmt.id : SOFTWARE_TRANSFORM_VTXFMT;
	key.blend = blendKey_.value;
	key.depth = depthStencilKey_.value & ((1ULL << 18) - 1);
	key.logic = !gstate.isModeClear() && pipelineState_.logicState.logicOpEnabled ? 16 | (uint32_t)pipelineState_.logicState.logicOp : 0;
	return GetOrCreatePipeline(key, hw ? &dec_->decFmt : nullptr);
}

DrawEngineDeko3D::Pipeline *DrawEngineDeko3D::GetOrCreatePipeline(const Deko3DPipelineKey &key, const DecVtxFormat *format) {
	Pipeline *cached = pipelines_.GetOrNull(key);
	if (cached) {
		return cached;
	}
	auto *vs = shaderManager_->GetVertexShaderFromID(key.vsid);
	auto *fs = shaderManager_->GetFragmentShaderFromID(key.fsid);
	if (vs->Failed() || fs->Failed()) {
		return nullptr;
	}
	Deko3DBlendKey blend{};
	blend.value = key.blend;
	Deko3DDepthStencilKey depth{};
	depth.value = key.depth;
	static constexpr Draw::Comparison compares[] = {
		Draw::Comparison::NEVER, Draw::Comparison::ALWAYS,	   Draw::Comparison::EQUAL,	  Draw::Comparison::NOT_EQUAL,
		Draw::Comparison::LESS,	 Draw::Comparison::LESS_EQUAL, Draw::Comparison::GREATER, Draw::Comparison::GREATER_EQUAL,
	};
	static constexpr Draw::StencilOp stencilOps[] = {
		Draw::StencilOp::KEEP,
		Draw::StencilOp::ZERO,
		Draw::StencilOp::REPLACE,
		Draw::StencilOp::INVERT,
		Draw::StencilOp::INCREMENT_AND_CLAMP,
		Draw::StencilOp::DECREMENT_AND_CLAMP,
		Draw::StencilOp::KEEP,
		Draw::StencilOp::KEEP,
	};
	Draw::DepthStencilStateDesc ds{};
	ds.depthTestEnabled = depth.depthTestEnable;
	ds.depthWriteEnabled = depth.depthWriteEnable;
	ds.depthCompare = compares[depth.depthCompare];
	ds.stencilEnabled = depth.stencilTestEnable;
	ds.stencil = {stencilOps[depth.stencilFail], stencilOps[depth.stencilPass], stencilOps[depth.stencilDepthFail],
				  compares[depth.stencilCompare]};
	Draw::BlendStateDesc bs{};
	bs.enabled = blend.blendEnable;
	bs.colorMask = blend.colorWriteMask;
	bs.srcCol = (Draw::BlendFactor)blend.srcColor;
	bs.dstCol = (Draw::BlendFactor)blend.dstColor;
	bs.srcAlpha = (Draw::BlendFactor)blend.srcAlpha;
	bs.dstAlpha = (Draw::BlendFactor)blend.dstAlpha;
	bs.eqCol = (Draw::BlendOp)blend.eqColor;
	bs.eqAlpha = (Draw::BlendOp)blend.eqAlpha;
	static constexpr Draw::LogicOp logicOps[] = {
		Draw::LogicOp::LOGIC_CLEAR,			Draw::LogicOp::LOGIC_AND,		   Draw::LogicOp::LOGIC_AND_REVERSE,
		Draw::LogicOp::LOGIC_COPY,			Draw::LogicOp::LOGIC_AND_INVERTED, Draw::LogicOp::LOGIC_NOOP,
		Draw::LogicOp::LOGIC_XOR,			Draw::LogicOp::LOGIC_OR,		   Draw::LogicOp::LOGIC_NOR,
		Draw::LogicOp::LOGIC_EQUIV,			Draw::LogicOp::LOGIC_INVERT,	   Draw::LogicOp::LOGIC_OR_REVERSE,
		Draw::LogicOp::LOGIC_COPY_INVERTED, Draw::LogicOp::LOGIC_OR_INVERTED,  Draw::LogicOp::LOGIC_NAND,
		Draw::LogicOp::LOGIC_SET,
	};
	bs.logicEnabled = (key.logic & 16) != 0;
	bs.logicOp = logicOps[key.logic & 15];
	Draw::RasterStateDesc rs{Draw::CullMode::NONE, Draw::Facing::CCW};
	Draw::PipelineDesc desc{};
	desc.prim = Draw::Primitive::TRIANGLE_LIST;
	desc.shaders = {vs->module, fs->module};
	desc.inputLayout = CreateLayout(draw_, format);
	desc.blend = draw_->CreateBlendState(bs);
	desc.depthStencil = draw_->CreateDepthStencilState(ds);
	desc.raster = draw_->CreateRasterState(rs);
	Pipeline *pipeline = draw_->CreateGraphicsPipeline(desc, "PSP GE");
	desc.inputLayout->Release();
	desc.blend->Release();
	desc.depthStencil->Release();
	desc.raster->Release();
	if (pipeline) {
		pipelines_.Insert(key, pipeline);
	}
	return pipeline;
}

uint32_t DrawEngineDeko3D::PipelineRecordSize() { return sizeof(Deko3DPipelineKey); }
bool DrawEngineDeko3D::SavePipelines(FILE *f) {
	bool ok = true;
	pipelines_.Iterate([&](const Deko3DPipelineKey &key, Pipeline *) { ok = ok && fwrite(&key, sizeof(key), 1, f) == 1; });
	return ok;
}
bool DrawEngineDeko3D::LoadPipelines(FILE *f, int count, bool) {
	if (count < 0 || count > 100000) {
		return false;
	}
	for (int i = 0; i < count; ++i) {
		Deko3DPipelineKey key{};
		if (fread(&key, sizeof(key), 1, f) != 1) {
			return false;
		}
		DecVtxFormat format{};
		const bool hw = key.vtxFmtId != SOFTWARE_TRANSFORM_VTXFMT;
		if (hw) {
			format.InitializeFromID(key.vtxFmtId);
		}
		if (!GetOrCreatePipeline(key, hw ? &format : nullptr)) {
			return false;
		}
	}
	return true;
}

void DrawEngineDeko3D::ConvertStateToKeys(int prim) {
	bool useBufferedRendering = framebufferManager_->UseBufferedRendering();

	if (gstate_c.IsDirty(DIRTY_BLEND_STATE)) {
		if (gstate.isModeClear()) {
			blendKey_.blendEnable = false;
			blendKey_.eqColor = (uint32_t)BlendEq::ADD;
			blendKey_.eqAlpha = (uint32_t)BlendEq::ADD;
			blendKey_.srcColor = (uint32_t)BlendFactor::ONE;
			blendKey_.srcAlpha = (uint32_t)BlendFactor::ONE;
			blendKey_.dstColor = (uint32_t)BlendFactor::ZERO;
			blendKey_.dstAlpha = (uint32_t)BlendFactor::ZERO;
			blendKey_.colorWriteMask = ClearModeChannelMask();
			dynState_.useBlendColor = false;
		} else {
			pipelineState_.Convert(draw_->GetShaderLanguageDesc().bitwiseOps, gstate_c.Use(GPU_USE_SHADER_BLENDING));
			GenericMaskState &maskState = pipelineState_.maskState;
			GenericBlendState &blendState = pipelineState_.blendState;

			if (pipelineState_.FramebufferRead() && useBufferedRendering) {
				ApplyFramebufferRead(&fboTexBindState_);
				// The shader takes over the responsibility for blending, so recompute.
				ApplyStencilReplaceAndLogicOpIgnoreBlend(blendState.replaceAlphaWithStencil, blendState);
				dirtyRequiresRecheck_ |= DIRTY_FRAGMENTSHADER_STATE;
				gstate_c.Dirty(DIRTY_FRAGMENTSHADER_STATE);
			} else if (fboTexBound_) {
				boundSecondary_ = nullptr;
				fboTexBound_ = false;
				dirtyRequiresRecheck_ |= DIRTY_FRAGMENTSHADER_STATE;
				gstate_c.Dirty(DIRTY_FRAGMENTSHADER_STATE);
			}

			if (blendState.blendEnabled) {
				blendKey_.blendEnable = true;
				blendKey_.eqColor = (uint32_t)blendState.eqColor;
				blendKey_.eqAlpha = (uint32_t)blendState.eqAlpha;
				blendKey_.srcColor = (uint32_t)blendState.srcColor;
				blendKey_.srcAlpha = (uint32_t)blendState.srcAlpha;
				blendKey_.dstColor = (uint32_t)blendState.dstColor;
				blendKey_.dstAlpha = (uint32_t)blendState.dstAlpha;
				if (blendState.dirtyShaderBlendFixValues) {
					dirtyRequiresRecheck_ |= DIRTY_SHADERBLEND;
					gstate_c.Dirty(DIRTY_SHADERBLEND);
				}
				dynState_.useBlendColor = blendState.useBlendColor;
				if (blendState.useBlendColor) {
					blendState.blendColorToFloat4(dynState_.blendColor);
				}
			} else {
				blendKey_.blendEnable = false;
				blendKey_.eqColor = (uint32_t)BlendEq::ADD;
				blendKey_.eqAlpha = (uint32_t)BlendEq::ADD;
				blendKey_.srcColor = (uint32_t)BlendFactor::ONE;
				blendKey_.srcAlpha = (uint32_t)BlendFactor::ONE;
				blendKey_.dstColor = (uint32_t)BlendFactor::ZERO;
				blendKey_.dstAlpha = (uint32_t)BlendFactor::ZERO;
				dynState_.useBlendColor = false;
			}
			blendKey_.colorWriteMask = maskState.channelMask;
		}
	}

	if (gstate_c.IsDirty(DIRTY_RASTER_STATE)) {
		bool wantCull = !gstate.isModeClear() && prim != GE_PRIM_RECTANGLES && prim > GE_PRIM_LINE_STRIP && gstate.isCullEnabled();
		dynState_.cull = wantCull ? (gstate.getCullMode() ? Draw::CullMode::FRONT : Draw::CullMode::BACK) : Draw::CullMode::NONE;
		if (gstate.isModeClear() || gstate.isModeThrough()) {
			dynState_.depthClamp = false;
		} else if (gstate.getDepthRangeMin() == 0 || gstate.getDepthRangeMax() == 65535) {
			dynState_.depthClamp = gstate.isDepthClipEnabled() && gstate_c.Use(GPU_USE_DEPTH_CLAMP);
		} else {
			dynState_.depthClamp = false;
		}
	}

	if (gstate_c.IsDirty(DIRTY_DEPTHSTENCIL_STATE)) {
		GenericStencilFuncState stencilState;
		ConvertStencilFuncState(stencilState);
		Deko3DDepthStencilKey &key = depthStencilKey_;
		key.value = 0;

		if (gstate.isModeClear()) {
			key.depthTestEnable = true;
			key.depthCompare = GE_COMP_ALWAYS;
			key.depthWriteEnable = gstate.isClearModeDepthMask();
			if (gstate.isClearModeAlphaMask()) {
				key.stencilTestEnable = true;
				key.stencilCompare = GE_COMP_ALWAYS;
				key.stencilPass = GE_STENCILOP_REPLACE;
				key.stencilFail = GE_STENCILOP_REPLACE;
				key.stencilDepthFail = GE_STENCILOP_REPLACE;
				key.writeMask = stencilState.writeMask;
				key.compareMask = 0xFF;
				// Overridden from software transform for clear rectangles.
				dynState_.stencilRef = 0xFF;
			}
		} else {
			if (!IsDepthTestEffectivelyDisabled()) {
				key.depthTestEnable = true;
				key.depthCompare = gstate.getDepthTestFunction();
				key.depthWriteEnable = gstate.isDepthWriteEnabled();
				UpdateEverUsedEqualDepth(gstate.getDepthTestFunction());
			}
			if (stencilState.enabled) {
				key.stencilTestEnable = true;
				key.stencilCompare = stencilState.testFunc;
				key.stencilPass = stencilState.zPass;
				key.stencilFail = stencilState.sFail;
				key.stencilDepthFail = stencilState.zFail;
				key.writeMask = stencilState.writeMask;
				key.compareMask = stencilState.testMask;
				dynState_.stencilRef = stencilState.testRef;

				// Nasty special case for Spongebob and similar, see StateMappingVulkan.cpp.
				if (SpongebobDepthInverseConditions(stencilState)) {
					pipelineState_.logicState.logicOpEnabled = false;
					blendKey_.blendEnable = true;
					blendKey_.eqAlpha = (uint32_t)BlendEq::ADD;
					blendKey_.eqColor = (uint32_t)BlendEq::ADD;
					blendKey_.srcColor = (uint32_t)BlendFactor::ZERO;
					blendKey_.dstColor = (uint32_t)BlendFactor::ZERO;
					blendKey_.srcAlpha = (uint32_t)BlendFactor::ZERO;
					blendKey_.dstAlpha = (uint32_t)BlendFactor::ZERO;
					blendKey_.colorWriteMask = 8;
					key.depthCompare = GE_COMP_LESS; // Inverse of GREATER_EQUAL
					key.stencilCompare = GE_COMP_ALWAYS;
					key.stencilPass = GE_STENCILOP_ZERO;
					key.stencilFail = GE_STENCILOP_ZERO;
					key.stencilDepthFail = GE_STENCILOP_KEEP;
					dirtyRequiresRecheck_ |= DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE;
					gstate_c.Dirty(DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE);
				}
			}
		}
	}

	if (gstate_c.IsDirty(DIRTY_VIEWPORTSCISSOR_STATE)) {
		ViewportAndScissor vpAndScissor;
		ConvertViewportAndScissor(framebufferManager_->GetDisplayLayoutConfigCopy(), useBufferedRendering,
								  framebufferManager_->GetRenderWidth(), framebufferManager_->GetRenderHeight(),
								  framebufferManager_->GetTargetBufferWidth(), framebufferManager_->GetTargetBufferHeight(), vpAndScissor);
		dynState_.viewport =
			Draw::Viewport{vpAndScissor.viewportX, vpAndScissor.viewportY, vpAndScissor.viewportW, vpAndScissor.viewportH, 0.0, 1.0};
		dynState_.scissor[0] = vpAndScissor.scissorX;
		dynState_.scissor[1] = vpAndScissor.scissorY;
		dynState_.scissor[2] = std::max(0, vpAndScissor.scissorW);
		dynState_.scissor[3] = std::max(0, vpAndScissor.scissorH);
	}
}

void DrawEngineDeko3D::BindShaderBlendTex() {
	if (!gstate.isModeClear() && fboTexBindState_ == FBO_TEX_COPY_BIND_TEX) {
		VirtualFramebuffer *curRenderVfb = framebufferManager_->GetCurrentRenderVFB();
		bool bindResult = framebufferManager_->BindFramebufferAsColorTexture(1, curRenderVfb, ShaderReadBindFlags(),
																			 Draw::ALL_LAYERS);
		_dbg_assert_(bindResult);
		boundSecondary_ = (Draw::Texture *)draw_->GetNativeObject(Draw::NativeObject::BOUND_TEXTURE1_IMAGEVIEW);
		fboTexBound_ = true;
		fboTexBindState_ = FBO_TEX_NONE;
		// Must dirty blend state here so we re-copy next time.  Example: Lunar's spell effects.
		dirtyRequiresRecheck_ |= DIRTY_BLEND_STATE;
	} else if (gstate.isModeClear() || !fboTexBound_) {
		boundSecondary_ = nullptr;
	}
}

bool DrawEngineDeko3D::ApplyState(Pipeline *pipeline, bool overrideStencil, uint8_t stencil) {
	if (!pipeline || dynState_.scissor[2] <= 0 || dynState_.scissor[3] <= 0) {
		return false;
	}
	draw_->BindPipeline(pipeline);
	draw_->SetStencilParams(overrideStencil ? stencil : dynState_.stencilRef, depthStencilKey_.writeMask, depthStencilKey_.compareMask);
	if (dynState_.useBlendColor) {
		draw_->SetBlendFactor(dynState_.blendColor);
	}
	Draw::Deko3DSetRaster(draw_, dynState_.cull, dynState_.depthClamp);
	draw_->SetViewport(dynState_.viewport);
	draw_->SetScissorRect(dynState_.scissor[0], dynState_.scissor[1], dynState_.scissor[2], dynState_.scissor[3]);
	return true;
}

void DrawEngineDeko3D::BindResources(bool pixelMapped) {
	uint64_t dirty = shaderManager_->UpdateUniforms(framebufferManager_->UseBufferedRendering(), pixelMapped);
	if ((dirty & DIRTY_BASE_UNIFORMS) || !baseUBO_ || uboFrame_ != draw_->GetFrameCount()) {
		Draw::Deko3DBindUniform(draw_, 3, &shaderManager_->BaseUniforms(), sizeof(UB_VS_FS_Base));
		baseUBO_ = true;
	}
	if ((dirty & DIRTY_LIGHT_UNIFORMS) || !lightUBO_ || uboFrame_ != draw_->GetFrameCount()) {
		Draw::Deko3DBindUniform(draw_, 4, &shaderManager_->LightUniforms(), sizeof(UB_VS_Lights));
		lightUBO_ = true;
	}
	uboFrame_ = draw_->GetFrameCount();
	Draw::Texture *textures[] = {textureCache_->curTexture, boundSecondary_, boundDepal_};
	Draw::SamplerState *samplers[] = {textureCache_->curSampler ? textureCache_->curSampler : samplerNearest_, samplerNearest_,
									  boundDepalSmoothed_ ? samplerLinear_ : samplerNearest_};
	draw_->BindTextures(0, 3, textures);
	draw_->BindSamplerStates(0, 3, samplers);
}

void DrawEngineDeko3D::DrawPrimitives(Draw::Primitive prim, uint64_t vertices, uint32_t size, int count, const uint16_t *indices) {
	Draw::Deko3DDraw(draw_, prim, vertices, size, count, indices);
}

void DrawEngineDeko3D::ResetAfterSkippedDraw() {
	indexGen.Reset();
	numDecodedVerts_ = 0;
	numDrawVerts_ = 0;
	numDrawInds_ = 0;
	vertexCountInDrawCalls_ = 0;
	expandedVertsInDrawCalls_ = 0;
	numVertsToDecode_ = 0;
	decodeIndsCounter_ = 0;
	decodeVertsCounter_ = 0;
	gstate_c.vertexFullAlpha = true;
}

void DrawEngineDeko3D::Flush() {
	if (!numDrawVerts_) {
		return;
	}
	PROFILE_THIS_SCOPE("Flush");

	GEPrimitiveType prim = prevPrim_;

	bool useHWTransform = CanUseHardwareTransform(prim);
	if (clipInfoFlags_ & ClipInfoFlags::Valid) {
		if (clipInfoFlags_ & ClipInfoFlags::SoftClipCull) {
			useHWTransform = false;
		}
	}
	if (clipInfoFlags_ != lastClipInfoFlags_) {
		ClipInfoFlags changed = (ClipInfoFlags)((u32)clipInfoFlags_ ^ (u32)lastClipInfoFlags_);
		if (changed & (ClipInfoFlags::DepthClampFragment | ClipInfoFlags::MinMaxZDiscard)) {
			gstate_c.Dirty(DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_RASTER_STATE);
		}
		if (changed & ClipInfoFlags::FlatZ) {
			gstate_c.Dirty(DIRTY_TEXTURE_PARAMS);
		}
		lastClipInfoFlags_ = clipInfoFlags_;
	}
	if (useHWTransform != lastUseHwTransform_) {
		gstate_c.Dirty(DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE | DIRTY_RASTER_STATE);
		lastUseHwTransform_ = useHWTransform;
	}

	auto updateVertexFullAlpha = [&]() {
		bool hasColor = (lastVType_ & GE_VTYPE_COL_MASK) != GE_VTYPE_COL_NONE;
		if (gstate.isModeThrough()) {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha && (hasColor || gstate.getMaterialAmbientA() == 255);
		} else {
			gstate_c.vertexFullAlpha = gstate_c.vertexFullAlpha &&
									   ((hasColor && (gstate.materialupdate & 1)) || gstate.getMaterialAmbientA() == 255) &&
									   (!gstate.isLightingEnabled() || gstate.getAmbientA() == 255);
		}
	};

	auto updatePipeline = [&](bool hwTransform) -> Pipeline * {
		if (!lastPipeline_ ||
			gstate_c.IsDirty(DIRTY_BLEND_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_RASTER_STATE | DIRTY_DEPTHSTENCIL_STATE |
							 DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE) ||
			prim != lastPrim_) {
			if (prim != lastPrim_ ||
				gstate_c.IsDirty(DIRTY_BLEND_STATE | DIRTY_VIEWPORTSCISSOR_STATE | DIRTY_RASTER_STATE | DIRTY_DEPTHSTENCIL_STATE)) {
				ConvertStateToKeys(prim);
			}
			VShaderID vsid;
			FShaderID fsid;
			shaderManager_->GetShaderIDs(prim, dec_->VertexType(), &vsid, &fsid, pipelineState_, hwTransform, clipInfoFlags_);
			// This might copy the framebuffer, which interrupts the render pass. So it goes before the pipeline lookup,
			// which depends on the render target.
			BindShaderBlendTex();
			lastPipeline_ = GetPipeline(vsid, fsid, hwTransform);
			gstate_c.Clean(DIRTY_BLEND_STATE | DIRTY_DEPTHSTENCIL_STATE | DIRTY_RASTER_STATE | DIRTY_VIEWPORTSCISSOR_STATE);
			gstate_c.Dirty(dirtyRequiresRecheck_);
			dirtyRequiresRecheck_ = 0;
		}
		lastPrim_ = prim;
		return lastPipeline_;
	};

	if (useHWTransform) {
		Draw::Deko3DUpload valloc;
		if (lastVType_ & GE_VTYPE_WEIGHT_MASK) {
			// If skinning, we're predecoding into "decoded". So make sure we're done, then push that content.
			DecodeVerts(dec_, decoded_);
			valloc = Draw::Deko3DPush(draw_, decoded_, numDecodedVerts_ * dec_->GetDecVtxFmt().stride, 16);
		} else {
			int vertsToDecode = ComputeNumVertsToDecode();
			valloc = Draw::Deko3DAllocate(draw_, vertsToDecode * dec_->GetDecVtxFmt().stride, 16);
			if (!valloc.ptr) {
				ResetAfterSkippedDraw();
				return;
			}
			DecodeVerts(dec_, valloc.ptr);
		}

		int vertexCount;
		int maxIndex;
		bool useElements;
		DecodeIndsAndGetData(&prim, &vertexCount, &maxIndex, &useElements, false);
		gpuStats.perFrame.numVertsDrawn += vertexCount;
		updateVertexFullAlpha();

		if (gstate_c.IsDirty(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS) && !gstate.isModeClear() && gstate.isTextureMapEnabled()) {
			gstate_c.Clean(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS);
			const TextureApplyResult textureResult = textureCache_->ApplyTexture(true);
			textureCache_->ApplySampler(textureResult, clipInfoFlags_ & ClipInfoFlags::FlatZ, false);
		} else if (gstate.getTextureAddress(0) == (gstate.getFrameBufRawAddress() | 0x04000000)) {
			// This catches the case of clearing a texture.
			gstate_c.Dirty(DIRTY_TEXTURE_IMAGE);
		}

		Pipeline *pipeline = updatePipeline(true);
		if (!pipeline || !ApplyState(pipeline, false, 0)) {
			ResetAfterSkippedDraw();
			return;
		}
		BindResources(false);
		Draw::Primitive nativePrim = prim == GE_PRIM_TRIANGLE_STRIP ? Draw::Primitive::TRIANGLE_STRIP : Draw::Primitive::TRIANGLE_LIST;
		DrawPrimitives(nativePrim, valloc.buffer, valloc.size, vertexCount, useElements ? decIndex_ : nullptr);
		if (useDepthRaster_) {
			DepthRasterSubmitRaw(prim, dec_, dec_->VertexType(), vertexCount);
		}
	} else {
		gpuStats.perFrame.numSoftTransformedDraws++;
		PROFILE_THIS_SCOPE("soft");

		DecodeVerts(dec_, decoded_);
		int vertexCount = DecodeInds();
		updateVertexFullAlpha();

		gpuStats.perFrame.numVertsDrawn += vertexCount;
		prim = IndexGenerator::GeneralPrim((GEPrimitiveType)drawInds_[0].prim);

		if (useDepthRaster_) {
			DepthRasterPredecoded(prim, decoded_, numDecodedVerts_, dec_, vertexCount);
		}

		u16 *inds = decIndex_;

		bool textureNeedsApply = false;
		TextureApplyResult textureResult;
		if (gstate_c.IsDirty(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS) && !gstate.isModeClear() && gstate.isTextureMapEnabled()) {
			gstate_c.Clean(DIRTY_TEXTURE_IMAGE | DIRTY_TEXTURE_PARAMS);
			gstate_c.dstSquared = false;
			textureResult = textureCache_->ApplyTexture(true);
			// NOTE: After this is set, we MUST call ApplySampler before returning.
			textureNeedsApply = true;
		} else if (gstate.getTextureAddress(0) == (gstate.getFrameBufRawAddress() | 0x04000000)) {
			gstate_c.Dirty(DIRTY_TEXTURE_IMAGE);
		}

		SoftwareTransformResult result{};
		SoftwareTransformParams params{};
		params.decoded = decoded_;
		params.transformed = transformed_;
		params.transformedExpanded = transformedExpanded_;
		// Clears through the render pass don't respect the scissor, so only allow them when buffered.
		params.allowClear = framebufferManager_->UseBufferedRendering();
		params.allowSeparateAlphaClear = false;
		params.everUsedEqualDepth = everUsedEqualDepth_;
		params.clipInfoFlags = clipInfoFlags_;
		params.trianglesReversed = BatchTrianglesReversed();

		const SoftwareTransformAction action =
			RunSoftwareTransform(params, prim, dec_->VertexType(), dec_->GetDecVtxFmt(), numDecodedVerts_, VERTEX_BUFFER_MAX, vertexCount,
								 inds, RemainingIndices(inds), &result);
		if (textureNeedsApply) {
			textureCache_->ApplySampler(textureResult, clipInfoFlags_ & ClipInfoFlags::FlatZ, result.pixelMapped);
			if (gstate_c.dstSquared) {
				gstate_c.Dirty(DIRTY_BLEND_STATE);
			}
		}
		if (action == SW_DRAW_INDEXED) {
			Pipeline *pipeline = updatePipeline(false);
			if (!pipeline || !ApplyState(pipeline, result.setStencil, result.stencilValue)) {
				ResetAfterSkippedDraw();
				return;
			}
			BindResources(result.pixelMapped);
			Draw::Deko3DUpload valloc = Draw::Deko3DPush(draw_, result.drawBuffer, result.drawVertexCount * sizeof(TransformedVertex), 16);
			DrawPrimitives(Draw::Primitive::TRIANGLE_LIST, valloc.buffer, valloc.size, result.drawIndexCount, inds);
		} else if (action == SW_CLEAR) {
			// Note: we won't get here if the clear is alpha but not color, or color but not alpha.
			bool clearColor = gstate.isClearModeColorMask();
			bool clearAlpha = gstate.isClearModeAlphaMask(); // and stencil
			bool clearDepth = gstate.isClearModeDepthMask();
			Draw::Aspect mask = Draw::Aspect::NO_BIT;
			if (clearColor || clearAlpha) {
				mask |= Draw::Aspect::COLOR_BIT;
			}
			if (clearDepth) {
				mask |= Draw::Aspect::DEPTH_BIT;
			}
			if (clearAlpha) {
				mask |= Draw::Aspect::STENCIL_BIT;
			}
			// The alpha channel and the stencil channel are shared on the PSP.
			draw_->Clear(mask, result.color, result.depth, result.color >> 24);
			if (gstate_c.Use(GPU_USE_CLEAR_RAM_HACK) && gstate.isClearModeColorMask() &&
				(gstate.isClearModeAlphaMask() || gstate.FrameBufFormat() == GE_FORMAT_565)) {
				int scissorX1 = gstate.getScissorX1();
				int scissorY1 = gstate.getScissorY1();
				int scissorX2 = gstate.getScissorX2() + 1;
				int scissorY2 = gstate.getScissorY2() + 1;
				framebufferManager_->ApplyClearToMemory(scissorX1, scissorY1, scissorX2, scissorY2, result.color);
			}
		}
	}

	ResetAfterDrawInline();
	framebufferManager_->SetColorUpdated(gstate_c.skipDrawReason);
	gpuCommon_->NotifyFlush();
}
