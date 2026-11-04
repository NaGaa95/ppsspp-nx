// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace SwitchLauncher {

struct TextBitmap {
	int width = 0;
	int height = 0;
	float advance = 0;
	std::vector<uint8_t> rgba;
};

enum class TextAlignment { Left, Center, Right };

// One UI thread owns the FreeType faces. Shaping and rasterization use the same
// fallback runs so measurements, accents and right-to-left text agree.
class TextEngine {
public:
	TextEngine();
	~TextEngine();
	bool AddFont(const void *data, size_t size);
	bool HasGlyph(uint32_t codepoint) const;
	float Measure(std::string_view text, int pixels);
	TextBitmap Render(std::string_view text, int pixels);
	TextBitmap RenderLines(std::string_view text, int pixels, TextAlignment alignment = TextAlignment::Left);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace SwitchLauncher
