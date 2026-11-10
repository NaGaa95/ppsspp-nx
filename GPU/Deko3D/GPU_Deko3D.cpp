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

#include "Common/Log.h"
#include "Common/Data/Text/StringWriter.h"
#include "Common/GPU/thin3d.h"
#include "Common/GPU/Deko3D/DekoContext.h"
#include "Common/File/FileUtil.h"
#include "Common/TimeUtil.h"
#include "Core/Config.h"
#include "Core/Reporting.h"
#include "Core/System.h"
#include "Core/ELF/ParamSFO.h"
#include "GPU/GPUState.h"
#include "GPU/Common/ShaderCommon.h"
#include "GPU/Deko3D/GPU_Deko3D.h"
#include "GPU/Deko3D/DrawEngineDeko3D.h"
#include "GPU/Deko3D/FramebufferManagerDeko3D.h"
#include "GPU/Deko3D/ShaderManagerDeko3D.h"
#include "GPU/Deko3D/TextureCacheDeko3D.h"

GPU_Deko3D::GPU_Deko3D(GraphicsContext *gfxCtx, Draw::DrawContext *draw) : GPUCommonHW(gfxCtx, draw) {
	gstate_c.SetUseFlags(CheckGPUFeatures());

	drawEngine_ = new DrawEngineDeko3D(draw);
	shaderManagerDeko3D_ = new ShaderManagerDeko3D(draw);
	framebufferManagerDeko3D_ = new FramebufferManagerDeko3D(draw);
	framebufferManager_ = framebufferManagerDeko3D_;
	textureCacheDeko3D_ = new TextureCacheDeko3D(draw, framebufferManager_->GetDraw2D());
	textureCache_ = textureCacheDeko3D_;
	drawEngineCommon_ = drawEngine_;
	shaderManager_ = shaderManagerDeko3D_;

	drawEngine_->SetGPUCommon(this);
	drawEngine_->SetTextureCache(textureCacheDeko3D_);
	drawEngine_->SetFramebufferManager(framebufferManagerDeko3D_);
	drawEngine_->SetShaderManager(shaderManagerDeko3D_);
	drawEngine_->Init();
	framebufferManagerDeko3D_->SetTextureCache(textureCacheDeko3D_);
	framebufferManagerDeko3D_->SetDrawEngine(drawEngine_);
	framebufferManagerDeko3D_->SetShaderManager(shaderManagerDeko3D_);
	framebufferManagerDeko3D_->Init(msaaLevel_);
	textureCacheDeko3D_->SetFramebufferManager(framebufferManagerDeko3D_);
	textureCacheDeko3D_->SetShaderManager(shaderManagerDeko3D_);
	textureCacheDeko3D_->SetDrawEngine(drawEngine_);

	UpdateCmdInfo();
	BuildReportingInfo();
	textureCache_->NotifyConfigChanged();

	std::string discID = g_paramSFO.GetDiscID();
	if (!discID.empty()) {
		File::CreateFullPath(GetSysDirectory(DIRECTORY_APP_CACHE));
		shaderCachePath_ = GetSysDirectory(DIRECTORY_APP_CACHE) / (discID + ".dekoshadercache");
		LoadCache(shaderCachePath_);
	}
}

namespace {

// Increase when the stored keys change meaning (Deko3DPipelineKey, the shader IDs, the formats).
constexpr uint32_t DEKO3D_CACHE_MAGIC = 0x4F4B4544; // "DEKO"
constexpr uint32_t DEKO3D_CACHE_VERSION = 5;

struct Deko3DCacheHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t useFlags;
	uint32_t pipelineRecordSize; // Catches layout changes that forgot the version.
	int32_t numVertexShaders;
	int32_t numFragmentShaders;
	int32_t numPipelines;
};

} // namespace

// Compile the cached shaders before the game starts.
void GPU_Deko3D::LoadCache(const Path &filename) {
	if (!g_Config.bShaderCache) {
		return;
	}
	FILE *f = File::OpenCFile(filename, "rb");
	if (!f) {
		return;
	}
	if (!VerifyShaderCacheChecksum(filename)) {
		WARN_LOG(Log::G3D, "Old or damaged Deko3D shader cache (no matching checksum) - rebuilding.");
		fclose(f);
		File::Delete(filename);
		return;
	}
	Deko3DCacheHeader header{};
	bool ok = fread(&header, sizeof(header), 1, f) == 1 && header.magic == DEKO3D_CACHE_MAGIC && header.version == DEKO3D_CACHE_VERSION &&
			  header.pipelineRecordSize == drawEngine_->PipelineRecordSize();
	if (ok && header.useFlags != gstate_c.GetUseFlags()) {
		// The IDs carry the flags they were made with, so this just means some of them won't be used.
		WARN_LOG(Log::G3D, "Deko3D shader cache useFlags mismatch, %08x, expected %08x", header.useFlags, gstate_c.GetUseFlags());
	}
	double start = time_now_d();
	ok = ok && shaderManagerDeko3D_->LoadCache(f, header.numVertexShaders, header.numFragmentShaders);
	ok = ok && drawEngine_->LoadPipelines(f, header.numPipelines, true);
	fclose(f);
	if (ok) {
		INFO_LOG(Log::G3D, "Loaded Deko3D shader cache: %d vertex and %d fragment shaders, %d pipelines in %0.1f ms",
				 header.numVertexShaders, header.numFragmentShaders, header.numPipelines, (time_now_d() - start) * 1000.0);
	} else {
		WARN_LOG(Log::G3D, "Incompatible or damaged Deko3D shader cache, deleting it");
		File::Delete(filename);
	}
}

void GPU_Deko3D::SaveCache(const Path &filename) {
	// After DeviceLost (which saved it), the shaders are gone. Don't overwrite the cache with nothing.
	if (!g_Config.bShaderCache || !filename.Valid() || !draw_) {
		return;
	}
	FILE *f = File::OpenCFile(filename, "wb");
	if (!f) {
		return;
	}
	Deko3DCacheHeader header{};
	header.magic = DEKO3D_CACHE_MAGIC;
	header.version = DEKO3D_CACHE_VERSION;
	header.useFlags = gstate_c.GetUseFlags();
	header.pipelineRecordSize = drawEngine_->PipelineRecordSize();
	header.numVertexShaders = shaderManagerDeko3D_->GetNumVertexShaders();
	header.numFragmentShaders = shaderManagerDeko3D_->GetNumFragmentShaders();
	header.numPipelines = drawEngine_->GetNumPipelines();
	bool ok = fwrite(&header, sizeof(header), 1, f) == 1;
	ok = ok && shaderManagerDeko3D_->SaveCache(f);
	ok = ok && drawEngine_->SavePipelines(f);
	fclose(f);
	ok = ok && AppendShaderCacheChecksum(filename);
	if (ok) {
		INFO_LOG(Log::G3D, "Saved Deko3D shader cache: %d vertex and %d fragment shaders, %d pipelines", header.numVertexShaders,
				 header.numFragmentShaders, header.numPipelines);
	} else {
		ERROR_LOG(Log::G3D, "Failed to write the Deko3D shader cache, disk full?");
		File::Delete(filename);
	}
}

GPU_Deko3D::~GPU_Deko3D() {
	SaveCache(shaderCachePath_);
	if (draw_) {
		draw_->Wait();
	}
	// The other managers are deleted in ~GPUCommonHW.
	delete drawEngine_;
	drawEngine_ = nullptr;
	drawEngineCommon_ = nullptr;
}

u32 GPU_Deko3D::CheckGPUFeatures() const {
	u32 features = GPUCommonHW::CheckGPUFeatures();
	uint32_t fmt4444 = draw_->GetDataFormatSupport(Draw::DataFormat::A4R4G4B4_UNORM_PACK16);
	uint32_t fmt1555 = draw_->GetDataFormatSupport(Draw::DataFormat::A1R5G5B5_UNORM_PACK16);
	uint32_t fmt565 = draw_->GetDataFormatSupport(Draw::DataFormat::R5G6B5_UNORM_PACK16);
	if ((fmt4444 & Draw::FMT_TEXTURE) && (fmt565 & Draw::FMT_TEXTURE) && (fmt1555 & Draw::FMT_TEXTURE)) {
		features |= GPU_USE_16BIT_FORMATS;
	}
	return CheckGPUFeaturesLate(features);
}

void GPU_Deko3D::BeginHostFrame(const DisplayLayoutConfig &config) {
	GPUCommonHW::BeginHostFrame(config);

	drawEngine_->BeginFrame();
	textureCache_->StartFrame();
	framebufferManager_->BeginFrame(config);

	gstate_c.Dirty(DIRTY_ALL);

	if (gstate_c.useFlagsChanged) {
		WARN_LOG(Log::G3D, "Shader use flags changed, clearing all shaders and depth buffers");
		drawEngine_->ClearPipelines();
		shaderManager_->ClearShaders();
		framebufferManager_->ClearAllDepthBuffers();
		gstate_c.useFlagsChanged = false;
	}
}

void GPU_Deko3D::FinishDeferred() { drawEngine_->FinishDeferred(); }

void GPU_Deko3D::DeviceLost() {
	// The managers free their caches (pipelines, samplers, depth stencil states) right away, so nothing
	// recorded can still be waiting to use them.
	if (draw_) {
		draw_->Wait();
	}
	SaveCache(shaderCachePath_);
	drawEngine_->ClearPipelines();
	GPUCommonHW::DeviceLost();
}

void GPU_Deko3D::DeviceRestore(Draw::DrawContext *draw) {
	GPUCommonHW::DeviceRestore(draw);

	// DeviceLost saved the cache and then threw everything away. Load it again, or the next save would
	// overwrite the file with only what gets drawn from now on.
	if (shaderCachePath_.Valid()) {
		LoadCache(shaderCachePath_);
	}
}

void GPU_Deko3D::GetStats(StringWriter &w) {
	w.F("Vertex, Fragment shaders, Pipelines loaded: %d, %d, %d\n", shaderManagerDeko3D_->GetNumVertexShaders(),
		shaderManagerDeko3D_->GetNumFragmentShaders(), drawEngine_->GetNumPipelines());
	FormatGPUStatsCommon(w);
}
