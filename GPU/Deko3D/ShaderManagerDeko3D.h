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

#include <cstdio>
#include <string>

#include "Common/Data/Collections/Hashmaps.h"
#include "Common/GPU/thin3d.h"
#include "GPU/Common/ShaderCommon.h"
#include "GPU/Common/ShaderId.h"
#include "GPU/Common/ShaderUniforms.h"
#include "GPU/Common/VertexShaderGenerator.h"
#include "GPU/Common/FragmentShaderGenerator.h"

enum class ClipInfoFlags;
struct ComputedPipelineState;

// Vulkan shader bindings are preserved through SPIR-V and the native shader compiler.
enum {
	DEKO3D_DRAW_BINDING_TEXTURE = 0,
	DEKO3D_DRAW_BINDING_2ND_TEXTURE = 1,
	DEKO3D_DRAW_BINDING_DEPAL_TEXTURE = 2,
	DEKO3D_DRAW_BINDING_DYNUBO_BASE = 3,
	DEKO3D_DRAW_BINDING_DYNUBO_LIGHT = 4,
};

class Deko3DShader {
public:
	Deko3DShader(Draw::DrawContext *draw, ShaderStage stage, const std::string &source, const std::string &desc);

	bool Failed() const { return module == nullptr; }
	~Deko3DShader() {
		if (module) {
			module->Release();
		}
	}
	std::string GetShaderString(DebugShaderStringType type) const;

	Draw::ShaderModule *module = nullptr;
	std::string source;
	std::string desc;
};

class Deko3DVertexShader : public Deko3DShader {
public:
	Deko3DVertexShader(Draw::DrawContext *draw, VShaderID vsid, VertexShaderFlags flags, const std::string &source)
		: Deko3DShader(draw, ShaderStage::Vertex, source, vsid.Description()), id_(vsid), flags_(flags) {}
	VShaderID id_;
	VertexShaderFlags flags_;
};

class Deko3DFragmentShader : public Deko3DShader {
public:
	Deko3DFragmentShader(Draw::DrawContext *draw, FShaderID fsid, FragmentShaderFlags flags, const std::string &source)
		: Deko3DShader(draw, ShaderStage::Fragment, source, fsid.Description()), id_(fsid), flags_(flags) {}
	FShaderID id_;
	FragmentShaderFlags flags_;
};

class ShaderManagerDeko3D : public ShaderManagerCommon {
public:
	ShaderManagerDeko3D(Draw::DrawContext *draw);
	~ShaderManagerDeko3D();

	void DeviceLost() override;
	void DeviceRestore(Draw::DrawContext *draw) override;
	void ClearShaders() override;

	void GetShaderIDs(int prim, u32 vertexType, VShaderID *vshader, FShaderID *fshader, const ComputedPipelineState &pipelineState,
					  bool useHWTransform, ClipInfoFlags clipInfoFlags);

	Deko3DVertexShader *GetVertexShaderFromID(VShaderID vsid);
	Deko3DFragmentShader *GetFragmentShaderFromID(FShaderID fsid);

	int GetNumVertexShaders() const { return (int)vsCache_.size(); }
	int GetNumFragmentShaders() const { return (int)fsCache_.size(); }

	std::vector<std::string> DebugGetShaderIDs(DebugShaderType type) override;
	std::string DebugGetShaderString(std::string id, DebugShaderType type, DebugShaderStringType stringType) override;

	// The shader cache: the IDs of this game's shaders.
	bool SaveCache(FILE *f);
	bool LoadCache(FILE *f, int numVertexShaders, int numFragmentShaders);

	// Returns the dirty uniform flags.
	uint64_t UpdateUniforms(bool useBufferedRendering, bool pixelMapped);
	const UB_VS_FS_Base &BaseUniforms() const { return ub_base_; }
	const UB_VS_Lights &LightUniforms() const { return ub_lights_; }

private:
	void Clear();

	ShaderLanguageDesc compat_;
	DenseHashMap<VShaderID, Deko3DVertexShader *> vsCache_;
	DenseHashMap<FShaderID, Deko3DFragmentShader *> fsCache_;
	char *codeBuffer_;

	UB_VS_FS_Base ub_base_{};
	UB_VS_Lights ub_lights_{};

	VShaderID lastVSID_;
	FShaderID lastFSID_;
};
