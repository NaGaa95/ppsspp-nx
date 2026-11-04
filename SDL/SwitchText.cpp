// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#include "SDL/SwitchText.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb.h>
#include <hb-ft.h>
#include <fribidi.h>

#include "Common/Data/Encoding/Utf8.h"

namespace SwitchLauncher {

struct TextEngine::Impl {
	struct Face {
		FT_Face ft = nullptr;
		hb_font_t *hb = nullptr;
		int pixels = 0;
		hb_script_t script = HB_SCRIPT_INVALID;
	};
	struct Glyph {
		size_t face;
		uint32_t index;
		float x, y;
	};
	struct Layout {
		std::vector<Glyph> glyphs;
		float advance = 0;
	};
	FT_Library library = nullptr;
	std::vector<Face> faces;

	~Impl() {
		for (auto &face : faces) {
			hb_font_destroy(face.hb);
			FT_Done_Face(face.ft);
		}
		if (library) {
			FT_Done_FreeType(library);
		}
	}

	size_t FindFace(uint32_t codepoint) const {
		const auto script = hb_unicode_script(hb_unicode_funcs_get_default(), codepoint);
		for (size_t i = 0; i < faces.size(); ++i) {
			if (faces[i].script == script && FT_Get_Char_Index(faces[i].ft, codepoint)) {
				return i;
			}
		}
		for (size_t i = 0; i < faces.size(); ++i) {
			if (FT_Get_Char_Index(faces[i].ft, codepoint)) {
				return i;
			}
		}
		return 0;
	}

	void SetSize(Face &face, int pixels) {
		if (face.pixels != pixels) {
			FT_Set_Pixel_Sizes(face.ft, 0, pixels);
			hb_ft_font_changed(face.hb);
			face.pixels = pixels;
		}
	}

	Layout Shape(std::string_view text, int pixels) {
		Layout result;
		if (faces.empty() || text.size() > 16384 || pixels <= 0) {
			return result;
		}
		std::vector<FriBidiChar> chars;
		UTF8 utf8(text);
		while (!utf8.end()) {
			chars.push_back(utf8.next());
		}
		if (chars.empty()) {
			return result;
		}
		std::vector<FriBidiCharType> types(chars.size());
		std::vector<FriBidiLevel> levels(chars.size());
		std::vector<FriBidiBracketType> brackets(chars.size());
		fribidi_get_bidi_types(chars.data(), (FriBidiStrIndex)chars.size(), types.data());
		fribidi_get_bracket_types(chars.data(), (FriBidiStrIndex)chars.size(), types.data(), brackets.data());
		FriBidiParType base = FRIBIDI_PAR_ON;
		if (!fribidi_get_par_embedding_levels_ex(types.data(), brackets.data(), (FriBidiStrIndex)chars.size(), &base, levels.data())) {
			std::fill(levels.begin(), levels.end(), 0);
		}
		struct Run {
			size_t start, end, face;
			hb_script_t script;
			FriBidiLevel level;
		};
		std::vector<Run> runs;
		hb_script_t script = HB_SCRIPT_LATIN;
		for (size_t i = 0; i < chars.size(); ++i) {
			const auto nextScript = hb_unicode_script(hb_unicode_funcs_get_default(), chars[i]);
			if (nextScript != HB_SCRIPT_COMMON && nextScript != HB_SCRIPT_INHERITED && nextScript != HB_SCRIPT_UNKNOWN) {
				script = nextScript;
			}
			const bool inherited = nextScript == HB_SCRIPT_INHERITED || chars[i] == 0x200c || chars[i] == 0x200d;
			const size_t face = inherited && !runs.empty() ? runs.back().face : FindFace(chars[i]);
			if (!runs.empty() && runs.back().face == face && runs.back().level == levels[i] && runs.back().script == script) {
				runs.back().end = i + 1;
			} else {
				runs.push_back({i, i + 1, face, script, levels[i]});
			}
		}
		// UAX #9 L2: reorder runs, leaving logical characters within each run for HarfBuzz.
		int maximum = 0, minimumOdd = 126;
		for (const Run &run : runs) {
			maximum = std::max(maximum, (int)run.level);
			if (run.level & 1) {
				minimumOdd = std::min(minimumOdd, (int)run.level);
			}
		}
		for (int level = maximum; level >= minimumOdd; --level) {
			for (size_t start = 0; start < runs.size();) {
				if (runs[start].level < level) {
					++start;
					continue;
				}
				size_t end = start + 1;
				while (end < runs.size() && runs[end].level >= level) {
					++end;
				}
				std::reverse(runs.begin() + start, runs.begin() + end);
				start = end;
			}
		}
		for (const Run &run : runs) {
			auto &face = faces[run.face];
			SetSize(face, pixels);
			hb_buffer_t *buffer = hb_buffer_create();
			hb_buffer_add_utf32(buffer, chars.data(), (int)chars.size(), (unsigned)run.start, (int)(run.end - run.start));
			hb_buffer_set_direction(buffer, run.level & 1 ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
			hb_buffer_set_script(buffer, run.script);
			hb_buffer_guess_segment_properties(buffer);
			hb_shape(face.hb, buffer, nullptr, 0);
			unsigned count = 0;
			const auto *info = hb_buffer_get_glyph_infos(buffer, &count);
			const auto *positions = hb_buffer_get_glyph_positions(buffer, &count);
			for (unsigned i = 0; i < count; ++i) {
				result.glyphs.push_back(
					{run.face, info[i].codepoint, result.advance + positions[i].x_offset / 64.f, -positions[i].y_offset / 64.f});
				result.advance += positions[i].x_advance / 64.f;
			}
			hb_buffer_destroy(buffer);
		}
		return result;
	}
};

TextEngine::TextEngine() : impl_(new Impl) { FT_Init_FreeType(&impl_->library); }
TextEngine::~TextEngine() = default;

bool TextEngine::AddFont(const void *data, size_t size) {
	if (!impl_->library || !data || size > (size_t)std::numeric_limits<FT_Long>::max()) {
		return false;
	}
	FT_Face face = nullptr;
	if (FT_New_Memory_Face(impl_->library, (const FT_Byte *)data, (FT_Long)size, 0, &face)) {
		return false;
	}
	hb_script_t script = HB_SCRIPT_INVALID;
	const std::string_view family = face->family_name ? face->family_name : "";
	for (const auto &entry : {std::pair{"Arabic", HB_SCRIPT_ARABIC},
							  {"Hebrew", HB_SCRIPT_HEBREW},
							  {"Khmer", HB_SCRIPT_KHMER},
							  {"Lao", HB_SCRIPT_LAO},
							  {"Thai", HB_SCRIPT_THAI}}) {
		if (family.find(entry.first) != std::string_view::npos) {
			script = entry.second;
		}
	}
	impl_->faces.push_back({face, hb_ft_font_create_referenced(face), 0, script});
	return true;
}

bool TextEngine::HasGlyph(uint32_t codepoint) const {
	return !impl_->faces.empty() && FT_Get_Char_Index(impl_->faces[impl_->FindFace(codepoint)].ft, codepoint) != 0;
}

float TextEngine::Measure(std::string_view text, int pixels) { return impl_->Shape(text, pixels).advance; }

TextBitmap TextEngine::Render(std::string_view text, int pixels) {
	TextBitmap result;
	const auto layout = impl_->Shape(text, pixels);
	result.advance = layout.advance;
	struct Bitmap {
		int x, y, width, height;
		std::vector<uint8_t> alpha;
	};
	std::vector<Bitmap> bitmaps;
	int left = 0, top = -pixels, right = (int)std::ceil(layout.advance), bottom = pixels / 3;
	for (const auto &glyph : layout.glyphs) {
		auto face = impl_->faces[glyph.face].ft;
		if (FT_Load_Glyph(face, glyph.index, FT_LOAD_DEFAULT) || FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) {
			continue;
		}
		const auto &source = face->glyph->bitmap;
		Bitmap bitmap{(int)std::lround(glyph.x) + face->glyph->bitmap_left,
					  (int)std::lround(glyph.y) - face->glyph->bitmap_top,
					  (int)source.width,
					  (int)source.rows,
					  {}};
		if (bitmap.width > 4096 || bitmap.height > 1024) {
			continue;
		}
		bitmap.alpha.resize((size_t)bitmap.width * bitmap.height);
		for (int y = 0; y < bitmap.height; ++y) {
			const uint8_t *row = source.buffer + (source.pitch < 0 ? bitmap.height - 1 - y : y) * std::abs(source.pitch);
			for (int x = 0; x < bitmap.width; ++x) {
				bitmap.alpha[y * bitmap.width + x] =
					source.pixel_mode == FT_PIXEL_MODE_MONO ? ((row[x / 8] >> (7 - x % 8)) & 1) * 255 : row[x];
			}
		}
		left = std::min(left, bitmap.x);
		top = std::min(top, bitmap.y);
		right = std::max(right, bitmap.x + bitmap.width);
		bottom = std::max(bottom, bitmap.y + bitmap.height);
		bitmaps.push_back(std::move(bitmap));
	}
	result.width = std::clamp(right - left, 1, 8192);
	result.height = std::clamp(bottom - top, 1, 1024);
	result.rgba.resize((size_t)result.width * result.height * 4, 255);
	for (size_t i = 3; i < result.rgba.size(); i += 4) {
		result.rgba[i] = 0;
	}
	for (const auto &bitmap : bitmaps) {
		for (int y = 0; y < bitmap.height; ++y) {
			for (int x = 0; x < bitmap.width; ++x) {
				const int destX = bitmap.x - left + x, destY = bitmap.y - top + y;
				if (destX < 0 || destY < 0 || destX >= result.width || destY >= result.height) {
					continue;
				}
				uint8_t &dest = result.rgba[((size_t)destY * result.width + destX) * 4 + 3];
				const uint8_t alpha = bitmap.alpha[y * bitmap.width + x];
				dest = alpha + (dest * (255 - alpha) + 127) / 255;
			}
		}
	}
	return result;
}

TextBitmap TextEngine::RenderLines(std::string_view text, int pixels, TextAlignment alignment) {
	std::vector<TextBitmap> lines;
	int lineHeight = std::max(1, pixels + pixels / 3), width = 1;
	while (!text.empty() && lines.size() < 4096 / (size_t)lineHeight) {
		const size_t end = text.find('\n');
		auto line = Render(text.substr(0, end), pixels);
		width = std::max(width, line.width);
		lineHeight = std::max(lineHeight, line.height);
		lines.push_back(std::move(line));
		if (end == std::string_view::npos) {
			break;
		}
		text.remove_prefix(end + 1);
		if (text.empty()) {
			lines.push_back(Render({}, pixels));
		}
	}
	TextBitmap result;
	result.width = width;
	result.height = std::min(4096, std::max(1, (int)lines.size() * lineHeight));
	result.rgba.resize((size_t)result.width * result.height * 4, 0);
	for (size_t index = 0; index < lines.size(); ++index) {
		const auto &line = lines[index];
		const int x = alignment == TextAlignment::Center  ? (width - line.width) / 2
					  : alignment == TextAlignment::Right ? width - line.width
														  : 0;
		for (int y = 0; y < line.height && (int)index * lineHeight + y < result.height; ++y) {
			std::copy_n(line.rgba.data() + (size_t)y * line.width * 4, line.width * 4,
						result.rgba.data() + ((index * lineHeight + y) * width + x) * 4);
		}
	}
	return result;
}

} // namespace SwitchLauncher
