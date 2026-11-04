#pragma once

#include "Common/UI/Context.h"
#include "Common/Render/TextureAtlas.h"

const Atlas *GetFontAtlas();
Atlas *GetUIAtlas();
AtlasData AtlasProvider(Draw::DrawContext *draw, AtlasChoice atlas, float dpiScale, bool invalidate);

struct Image;
bool RasterizeSVGImage(std::string_view filename, std::string_view id, float scale, Image *output);
