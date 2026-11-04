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

#include "Common/File/Path.h"
#include "GPU/GPUCommonHW.h"

class FramebufferManagerDeko3D;
class ShaderManagerDeko3D;
class TextureCacheDeko3D;
class DrawEngineDeko3D;
class StringWriter;

class GPU_Deko3D : public GPUCommonHW {
public:
	GPU_Deko3D(GraphicsContext *gfxCtx, Draw::DrawContext *draw);
	~GPU_Deko3D();

	u32 CheckGPUFeatures() const override;

	void BeginHostFrame(const DisplayLayoutConfig &config) override;

	void GetStats(StringWriter &w) override;
	void DeviceLost() override;
	void DeviceRestore(Draw::DrawContext *draw) override;

protected:
	void FinishDeferred() override;

private:
	void LoadCache(const Path &filename);
	void SaveCache(const Path &filename);

	Path shaderCachePath_;
	FramebufferManagerDeko3D *framebufferManagerDeko3D_;
	TextureCacheDeko3D *textureCacheDeko3D_;
	ShaderManagerDeko3D *shaderManagerDeko3D_;
	DrawEngineDeko3D *drawEngine_;
};
