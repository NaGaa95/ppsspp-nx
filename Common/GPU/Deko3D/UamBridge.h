// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <cstddef>

extern "C" {

enum UamStage {
	UamStage_Vertex = 0,
	UamStage_TessControl = 1,
	UamStage_TessEval = 2,
	UamStage_Geometry = 3,
	UamStage_Fragment = 4,
	UamStage_Compute = 5,
};

void *UamCompileGlsl(const char *glsl, int stage, size_t *out_size, char **out_log);
// Also returns how many clip and cull distances the shader writes. The hardware puts the cull distances
// after the clip distances.
void *UamCompileGlslDistances(const char *glsl, int stage, size_t *out_size, char **out_log, unsigned *out_clip_distances,
							  unsigned *out_cull_distances);
}
