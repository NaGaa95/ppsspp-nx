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
#include "Common/LogReporting.h"
#include "Common/Profiler/Profiler.h"
#include "Common/StringUtils.h"
#include "Common/GPU/thin3d.h"
#include "Common/GPU/Deko3D/DekoContext.h"
#include "GPU/GPUState.h"
#include "GPU/Common/DrawEngineCommon.h"
#include "GPU/Deko3D/ShaderManagerDeko3D.h"

static constexpr size_t CODE_BUFFER_SIZE = 32768;

Deko3DShader::Deko3DShader(Draw::DrawContext *draw, ShaderStage stage, const std::string &src, const std::string &description)
	: source(src), desc(description) {
	PROFILE_THIS_SCOPE("shadercomp");
	ShaderStage drawStage = stage == ShaderStage::Vertex ? ShaderStage::Vertex : ShaderStage::Fragment;
	module = Draw::Deko3DCreateEmulationShader(draw, drawStage, source, desc.c_str());
	if (!module) {
		ERROR_LOG(Log::G3D, "Failed to compile Deko3D shader (%s):\n%s", desc.c_str(), LineNumberString(source).c_str());
	}
}

std::string Deko3DShader::GetShaderString(DebugShaderStringType type) const {
	switch (type) {
	case SHADER_STRING_SOURCE_CODE:
		return source;
	case SHADER_STRING_SHORT_DESC:
		return desc;
	default:
		return "N/A";
	}
}

ShaderManagerDeko3D::ShaderManagerDeko3D(Draw::DrawContext *draw)
	: ShaderManagerCommon(draw), compat_(GLSL_VULKAN), vsCache_(16), fsCache_(16) {
	codeBuffer_ = new char[CODE_BUFFER_SIZE];
}

ShaderManagerDeko3D::~ShaderManagerDeko3D() {
	Clear();
	delete[] codeBuffer_;
}

void ShaderManagerDeko3D::DeviceLost() {
	Clear();
	draw_ = nullptr;
}

void ShaderManagerDeko3D::DeviceRestore(Draw::DrawContext *draw) { draw_ = draw; }

void ShaderManagerDeko3D::Clear() {
	vsCache_.Iterate([](const VShaderID &, Deko3DVertexShader *shader) { delete shader; });
	fsCache_.Iterate([](const FShaderID &, Deko3DFragmentShader *shader) { delete shader; });
	vsCache_.Clear();
	fsCache_.Clear();
	lastVSID_.set_invalid();
	lastFSID_.set_invalid();
	gstate_c.Dirty(DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE);
}

void ShaderManagerDeko3D::ClearShaders() {
	Clear();
	gstate_c.Dirty(DIRTY_ALL_UNIFORMS | DIRTY_VERTEXSHADER_STATE | DIRTY_FRAGMENTSHADER_STATE);
}

uint64_t ShaderManagerDeko3D::UpdateUniforms(bool useBufferedRendering, bool pixelMapped) {
	DirtyDepalIfPixelMappedChanged(pixelMapped);
	uint64_t dirty = gstate_c.GetDirtyUniforms();
	if (dirty != 0) {
		if (dirty & DIRTY_BASE_UNIFORMS) {
			BaseUpdateUniforms(&ub_base_, dirty, useBufferedRendering, pixelMapped);
		}
		if (dirty & DIRTY_LIGHT_UNIFORMS) {
			LightUpdateUniforms(&ub_lights_, dirty);
		}
	}
	gstate_c.CleanUniforms();
	return dirty;
}

Deko3DVertexShader *ShaderManagerDeko3D::GetVertexShaderFromID(VShaderID vsid) {
	Deko3DVertexShader *vs = vsCache_.GetOrNull(vsid);
	if (vs) {
		return vs;
	}
	std::string genErrorString;
	uint64_t uniformMask = 0;
	uint32_t attributeMask = 0;
	VertexShaderFlags flags{};
	bool success =
		GenerateVertexShader(vsid, codeBuffer_, compat_, draw_->GetBugs(), &attributeMask, &uniformMask, &flags, &genErrorString);
	_assert_msg_(success, "VS gen error: %s", genErrorString.c_str());
	vs = new Deko3DVertexShader(draw_, vsid, flags, codeBuffer_);
	vsCache_.Insert(vsid, vs);
	return vs;
}

Deko3DFragmentShader *ShaderManagerDeko3D::GetFragmentShaderFromID(FShaderID fsid) {
	Deko3DFragmentShader *fs = fsCache_.GetOrNull(fsid);
	if (fs) {
		return fs;
	}
	std::string genErrorString;
	uint64_t uniformMask = 0;
	FragmentShaderFlags flags{};
	bool success = GenerateFragmentShader(fsid, codeBuffer_, compat_, draw_->GetBugs(), &uniformMask, &flags, &genErrorString);
	_assert_msg_(success, "FS gen error: %s", genErrorString.c_str());
	fs = new Deko3DFragmentShader(draw_, fsid, flags, codeBuffer_);
	fsCache_.Insert(fsid, fs);
	return fs;
}

void ShaderManagerDeko3D::GetShaderIDs(int prim, u32 vertexType, VShaderID *vshader, FShaderID *fshader,
									   const ComputedPipelineState &pipelineState, bool useHWTransform, ClipInfoFlags clipInfoFlags) {
	if (gstate_c.IsDirty(DIRTY_VERTEXSHADER_STATE)) {
		gstate_c.Clean(DIRTY_VERTEXSHADER_STATE);
		ComputeVertexShaderID(&lastVSID_, vertexType, useHWTransform, clipInfoFlags);
	}
	*vshader = lastVSID_;

	if (gstate_c.IsDirty(DIRTY_FRAGMENTSHADER_STATE)) {
		gstate_c.Clean(DIRTY_FRAGMENTSHADER_STATE);
		ComputeFragmentShaderID(&lastFSID_, pipelineState, draw_->GetBugs(), clipInfoFlags);
	}
	*fshader = lastFSID_;

	_dbg_assert_(lastFSID_.Bit(FS_BIT_FLATSHADE) == lastVSID_.Bit(VS_BIT_FLATSHADE));
	_dbg_assert_msg_(lastVSID_.Bit(VS_BIT_USE_HW_TRANSFORM) == useHWTransform, "Bad vshader ID was computed");
}

bool ShaderManagerDeko3D::SaveCache(FILE *f) {
	bool ok = true;
	vsCache_.Iterate([&](const VShaderID &id, Deko3DVertexShader *) { ok = ok && fwrite(&id, sizeof(id), 1, f) == 1; });
	fsCache_.Iterate([&](const FShaderID &id, Deko3DFragmentShader *) { ok = ok && fwrite(&id, sizeof(id), 1, f) == 1; });
	return ok;
}

bool ShaderManagerDeko3D::LoadCache(FILE *f, int numVertexShaders, int numFragmentShaders) {
	if (numVertexShaders < 0 || numVertexShaders > 100000 || numFragmentShaders < 0 || numFragmentShaders > 100000) {
		return false;
	}
	// Compile before emulation begins to avoid recompilation during draws.
	for (int i = 0; i < numVertexShaders; i++) {
		VShaderID id;
		if (fread(&id, sizeof(id), 1, f) != 1) {
			return false;
		}
		GetVertexShaderFromID(id);
	}
	for (int i = 0; i < numFragmentShaders; i++) {
		FShaderID id;
		if (fread(&id, sizeof(id), 1, f) != 1) {
			return false;
		}
		GetFragmentShaderFromID(id);
	}
	return true;
}

std::vector<std::string> ShaderManagerDeko3D::DebugGetShaderIDs(DebugShaderType type) {
	std::vector<uint64_t> ids;
	switch (type) {
	case SHADER_TYPE_VERTEX:
		vsCache_.Iterate([&](const VShaderID &id, Deko3DVertexShader *) { ids.push_back(id.ToUint64()); });
		break;
	case SHADER_TYPE_FRAGMENT:
		fsCache_.Iterate([&](const FShaderID &id, Deko3DFragmentShader *) { ids.push_back(id.ToUint64()); });
		break;
	default:
		break;
	}
	return ToSortedDebugShaderIdVec(ids);
}

std::string ShaderManagerDeko3D::DebugGetShaderString(std::string idStr, DebugShaderType type, DebugShaderStringType stringType) {
	ShaderID shaderId;
	shaderId.FromString(idStr);
	switch (type) {
	case SHADER_TYPE_VERTEX: {
		Deko3DVertexShader *vs = vsCache_.GetOrNull(VShaderID(shaderId));
		return vs ? vs->GetShaderString(stringType) : "";
	}
	case SHADER_TYPE_FRAGMENT: {
		Deko3DFragmentShader *fs = fsCache_.GetOrNull(FShaderID(shaderId));
		return fs ? fs->GetShaderString(stringType) : "";
	}
	default:
		return "N/A";
	}
}
