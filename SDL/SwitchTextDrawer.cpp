// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#include "SDL/SwitchTextDrawer.h"
#include "SDL/SwitchText.h"
#include "SDL/SwitchLauncherUtil.h"

#include <algorithm>
#include <cmath>
#include <switch.h>

#include "Common/Render/Text/draw_text.h"
#include "Common/File/VFS/VFS.h"
#include "Common/GPU/thin3d.h"
#include "Common/StringUtils.h"
#include "Core/Config.h"

namespace {

class SwitchTextDrawer final : public TextDrawer {
public:
	explicit SwitchTextDrawer(Draw::DrawContext *draw) : TextDrawer(draw) {
		sharedFonts_ = R_SUCCEEDED(plInitialize(PlServiceType_User));
		PlSharedFontType preferred = PlSharedFontType_Standard;
		if (g_Config.sLanguageIni == "zh_CN") {
			preferred = PlSharedFontType_ChineseSimplified;
		}
		if (g_Config.sLanguageIni == "zh_TW") {
			preferred = PlSharedFontType_ChineseTraditional;
		}
		if (g_Config.sLanguageIni == "ko_KR") {
			preferred = PlSharedFontType_KO;
		}
		for (int i = -1; i < PlSharedFontType_Total; ++i) {
			const auto type = i < 0 ? preferred : (PlSharedFontType)i;
			if (i >= 0 && type == preferred) {
				continue;
			}
			PlFontData data{};
			if (sharedFonts_ && R_SUCCEEDED(plGetSharedFontByType(&data, type))) {
				ready_ |= engine_->AddFont(data.address, data.size);
			}
		}
		for (const char *name : {"NotoSans", "NotoSansArabic", "NotoSansHebrew", "NotoSansKhmer", "NotoSansLao", "NotoSansThai",
								 "NotoSansSymbols", "NotoSansSymbols2"}) {
			size_t size = 0;
			std::unique_ptr<uint8_t[]> data(g_VFS.ReadFile((std::string("launcher/fonts/") + name + "-Regular.ttf").c_str(), &size));
			if (data && size) {
				fontData_.emplace_back(data.get(), data.get() + size);
				ready_ |= engine_->AddFont(fontData_.back().data(), fontData_.back().size());
			}
		}
	}
	~SwitchTextDrawer() override {
		ClearCache();
		engine_.reset();
		if (sharedFonts_) {
			plExit();
		}
	}
	bool IsReady() const override { return ready_; }

protected:
	void EnsureFont(const FontStyle &) override {}
	void ClearFonts() override {}
	bool SupportsColorEmoji() const override { return false; }
	int Pixels(const FontStyle &style) const { return std::max(1, (int)std::lround(style.sizePts / dpiScale_)); }
	void MeasureStringInternal(const FontStyle &style, std::string_view text, float *width, float *height) override {
		const auto bitmap = engine_->RenderLines(text, Pixels(style));
		*width = bitmap.width;
		*height = bitmap.height;
	}
	bool DrawStringBitmap(const FontStyle &style, std::vector<uint8_t> &output, TextStringEntry &entry, Draw::DataFormat format,
						  std::string_view text, int align, bool fullColor) override {
		if (text.empty() || fullColor) {
			return false;
		}
		const auto alignment = align & ALIGN_HCENTER ? SwitchLauncher::TextAlignment::Center
							   : align & ALIGN_RIGHT ? SwitchLauncher::TextAlignment::Right
													 : SwitchLauncher::TextAlignment::Left;
		const std::string label = align & FLAG_NO_PREFIX ? std::string(text) : SwitchLauncher::WithoutMnemonics(text);
		const auto bitmap = engine_->RenderLines(label, Pixels(style), alignment);
		entry.width = bitmap.width;
		entry.height = bitmap.height;
		entry.bmWidth = (bitmap.width + 3) & ~3;
		entry.bmHeight = bitmap.height;
		entry.lastUsedFrame = frameCount_;
		const bool r8 = format == Draw::DataFormat::R8_UNORM;
		const bool rgba = format == Draw::DataFormat::R8G8B8A8_UNORM;
		const int bytes = r8 ? 1 : rgba ? 4 : 2;
		output.assign((size_t)entry.bmWidth * entry.bmHeight * bytes, 0);
		for (int y = 0; y < bitmap.height; ++y) {
			for (int column = 0; column < bitmap.width; ++column) {
				const uint8_t alpha = bitmap.rgba[((size_t)y * bitmap.width + column) * 4 + 3];
				const size_t offset = ((size_t)y * entry.bmWidth + column) * bytes;
				if (r8) {
					output[offset] = alpha;
				} else if (rgba) {
					std::fill_n(output.begin() + offset, 4, alpha);
				} else {
					const uint16_t value = AlphaToPremul4444(alpha);
					output[offset] = (uint8_t)value;
					output[offset + 1] = (uint8_t)(value >> 8);
				}
			}
		}
		return true;
	}

private:
	std::vector<std::vector<uint8_t>> fontData_;
	std::unique_ptr<SwitchLauncher::TextEngine> engine_ = std::make_unique<SwitchLauncher::TextEngine>();
	bool sharedFonts_ = false;
	bool ready_ = false;
};

} // namespace

TextDrawer *CreateSwitchTextDrawer(Draw::DrawContext *draw) {
	auto *drawer = new SwitchTextDrawer(draw);
	if (!drawer->IsReady()) {
		delete drawer;
		return nullptr;
	}
	return drawer;
}
