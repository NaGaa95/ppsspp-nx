// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <string>
#include "Common/GPU/Shader.h"

// Produces a self-contained DKSH program. The binding masks are used by the native descriptor cache.
// Sampled-image bindings are rebased separately from UBO/SSBO/image bindings.
// clipDistances and cullDistances, if given, get how many of each the compiled shader writes.
bool CompileDeko3DShader(ShaderStage stage, const char *source, std::string *binary, uint32_t *arrayTextures, uint32_t *textures,
						 std::string *error, uint32_t textureBindingBase = 0, uint32_t *clipDistances = nullptr,
						 uint32_t *cullDistances = nullptr);

// How many clip and cull distances GLSL source writes: the highest index used plus one, or a redeclared size.
void Deko3DCountDistances(const char *source, uint32_t *clipDistances, uint32_t *cullDistances);
// The Maxwell 3D CLIP_DISTANCE_MODE value for a vertex shader. deko3d never sets it, which leaves every
// distance clipping. The compiler puts the cull distances after the clip distances, and each needs its
// 4-bit field set to 1 (cull), as nouveau's nvc0_program.c does.
uint32_t Deko3DClipDistanceMode(uint32_t clipDistances, uint32_t cullDistances);
