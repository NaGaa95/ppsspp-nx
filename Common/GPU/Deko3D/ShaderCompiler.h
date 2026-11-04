// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <string>
#include "Common/GPU/Shader.h"

// Produces a self-contained DKSH program. The binding masks are used by the native descriptor cache.
// Sampled-image bindings are rebased separately from UBO/SSBO/image bindings.
bool CompileDeko3DShader(ShaderStage stage, const char *source, std::string *binary, uint32_t *arrayTextures, uint32_t *textures,
						 std::string *error, uint32_t textureBindingBase = 0);
