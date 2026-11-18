// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// Switch 2 UamBridge: GLSL compiles with uam2rt (drako3d's UAM2), whose DKSH is complete as returned; it also reports
// the clip and cull distances. Uniform block binding N is constant buffer N + 3, so graphics stages have bindings 0-14.

#include <cstdlib>
#include <cstring>
#include <mutex>

#include <uam2rt.h>

#include "Common/GPU/Deko3D/UamBridge.h"

static_assert(int(UamStage_Vertex) == int(Uam2rtStage_Vertex) && int(UamStage_TessControl) == int(Uam2rtStage_TessCtrl) &&
				  int(UamStage_TessEval) == int(Uam2rtStage_TessEval) && int(UamStage_Geometry) == int(Uam2rtStage_Geometry) &&
				  int(UamStage_Fragment) == int(Uam2rtStage_Fragment) && int(UamStage_Compute) == int(Uam2rtStage_Compute),
			  "uam2rt numbers the stages as uam");

namespace {
// malloc'd, as UamCompileGlsl's callers free the image and the log with free().
void *Copy(const void *data, size_t size) {
	void *copy = std::malloc(size ? size : 1);
	if (copy && size)
		std::memcpy(copy, data, size);
	return copy;
}
}  // namespace

void *UamCompileGlsl(const char *glsl, int stage, size_t *out_size, char **out_log) {
	return UamCompileGlslDistances(glsl, stage, out_size, out_log, nullptr, nullptr);
}

void *UamCompileGlslDistances(const char *glsl, int stage, size_t *out_size, char **out_log, unsigned *out_clip_distances,
							  unsigned *out_cull_distances) {
	*out_size = 0;
	*out_log = nullptr;
	size_t size = 0;
	char *log = nullptr;
	Uam2rtInfo info{};
	// The emulator and the frame generator's shader preparation compile from different threads.
	static std::mutex compilerMutex;
	std::unique_lock<std::mutex> guard(compilerMutex);
	void *image = uam2rtCompileGlsl(glsl, stage, &size, &log, &info);
	guard.unlock();
	if (log) {
		*out_log = (char *)Copy(log, std::strlen(log) + 1);
		uam2rtFree(log);
	}
	if (!image) {
		return nullptr;
	}
	void *result = Copy(image, size);
	uam2rtFree(image);
	if (!result)
		return nullptr;
	*out_size = size;
	if (out_clip_distances)
		*out_clip_distances = info.clip_distances;
	if (out_cull_distances)
		*out_cull_distances = info.cull_distances;
	return result;
}
