// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#include "UI/SwitchUIStyle.h"
#include "Common/SwitchUIColors.h"
#include "Common/UI/Context.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"
#include "Common/Render/DrawBuffer.h"
#include "Common/Data/Text/I18n.h"
#include "Core/Config.h"
#include "UI/GameInfoCache.h"
#include "UI/MiscViews.h"
#include "Common/GPU/Vulkan/SwitchLSFG.h"
#include "Core/HLE/sceUtility.h"
#include <algorithm>

namespace SwitchUI {

void ApplyTheme(UI::Theme &theme) {
	auto style = [](uint32_t text, uint32_t background) {
		UI::Style result;
		result.fgColor = text;
		result.background = UI::Drawable(background);
		return result;
	};
	theme.itemStyle = style(TEXT, Color(16, 23, 39, 210));
	theme.itemFocusedStyle = style(VALUE, FOCUS);
	theme.itemDownStyle = style(VALUE, Color(35, 88, 116));
	theme.itemDisabledStyle = style(Color(100, 110, 125), Color(16, 23, 39, 160));
	theme.headerStyle = style(ACCENT, Color(0, 0, 0, 0));
	theme.collapsibleHeaderStyle = style(ACCENT, PANEL);
	theme.infoStyle = style(DIM, Color(0, 0, 0, 0));
	theme.popupStyle = style(TEXT, Color(16, 23, 39, 250));
	theme.popupTitleStyle = style(VALUE, Color(16, 23, 39, 250));
	theme.tooltipStyle = style(TEXT, PANEL);
	theme.backgroundColor = Color(10, 16, 28);
	theme.scrollbarColor = ACCENT;
	theme.popupSliderColor = DIM;
	theme.popupSliderFocusedColor = ACCENT;
	theme.choiceRoundRadius = 8;
}

void DrawBackdrop(UIContext &dc) {
	const Bounds bounds = dc.GetBounds();
	dc.FillRect(UI::Drawable(Color(6, 12, 23, 115)), bounds);
	dc.Draw()->DrawImageStretchVGradient(ImageID("I_SOLIDWHITE"), bounds.x, bounds.y, bounds.x2(), bounds.y + 160, Color(15, 40, 58, 140),
										 Color(15, 40, 58, 0));
	const float inset = 26;
	dc.FillRect(UI::Drawable(Color(61, 183, 235, 75)), Bounds(bounds.x + inset, bounds.y + 82, bounds.w - inset * 2, 1));
}

class StatusView final : public UI::InertView {
public:
	StatusView() : InertView(new UI::LinearLayoutParams(UI::WRAP_CONTENT, 44)) {}
	std::string DescribeText() const override { return "LSFG"; }
	void GetContentDimensions(const UIContext &dc, float &w, float &h) const override {
		float onWidth, offWidth, textHeight;
		auto di = GetI18NCategory(I18NCat::DIALOG);
		dc.MeasureText(dc.GetTheme().uiFontSmall, 1.f, 1.f, di->T("On"), &onWidth, &textHeight);
		dc.MeasureText(dc.GetTheme().uiFontSmall, 1.f, 1.f, di->T("Off"), &offWidth, &textHeight);
		w = 36 + std::max(onWidth, offWidth);
		h = 44;
	}
	void Draw(UIContext &dc) override {
		const auto b = GetBounds();
		dc.SetFontStyle(dc.GetTheme().uiFontSmall);
		const bool active = SwitchLSFG_IsEnabled();
		dc.Draw()->DrawImageStretch(ImageID("I_LSFG_LOGO"), b.x, b.centerY() - 14, b.x + 28, b.centerY() + 14, 0xFFFFFFFF);
		dc.DrawText(GetI18NCategory(I18NCat::DIALOG)->T(active ? "On" : "Off"), b.x + 36, b.centerY(), StatusColor(active), ALIGN_VCENTER);
	}
};

class HeaderView final : public UI::LinearLayout {
public:
	HeaderView(const Path &path, std::string_view title, std::string_view subtitle, bool gameStatus, std::function<void()> info)
		: LinearLayout(ORIENT_HORIZONTAL, new UI::LinearLayoutParams(UI::FILL_PARENT, 76)) {
		using namespace UI;
		SetPadding(Margins(8, 0, 8, 4));
		if (info) {
			auto *button = Add(new Choice(ImageID("I_INFO"), new LinearLayoutParams(44, 44, 0.0f, Gravity::G_VCENTER)));
			button->SetRoundCorners();
			button->OnClick.Add([info = std::move(info)](EventParams &) { info(); });
		}
		SetSpacing(18);
		if (!path.empty()) {
			Add(new GameImageView(path, GameInfoFlags::ICON, .5f, new LinearLayoutParams(100, 56, 0.0f, Gravity::G_VCENTER)));
		}
		auto *labels = Add(new LinearLayout(ORIENT_VERTICAL, new LinearLayoutParams(1.0f)));
		labels->SetSpacing(4);
		auto *heading = labels->Add(
			new TextView(title, FLAG_ELLIPSIZE_TEXT | FLAG_NO_PREFIX, false, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
		heading->SetBig(true);
		heading->SetTextColor(VALUE);
		auto *context = labels->Add(new TextView(subtitle, FLAG_ELLIPSIZE_TEXT, true, new LinearLayoutParams(FILL_PARENT, WRAP_CONTENT)));
		context->SetTextColor(DIM);
		if (gameStatus) {
			Add(new StatusView());
		}
	}
};

UI::View *Header(const Path &path, std::string_view title, std::string_view subtitle, bool gameStatus, std::function<void()> info) {
	return new HeaderView(path, title, subtitle, gameStatus, std::move(info));
}

class FooterView final : public UI::InertView {
public:
	FooterView(bool resume, bool categories)
		: InertView(new UI::LinearLayoutParams(UI::FILL_PARENT, 42)), resume_(resume), categories_(categories) {}
	std::string DescribeText() const override { return "Controller hints"; }
	void GetContentDimensions(const UIContext &, float &w, float &h) const override {
		w = 500;
		h = 42;
	}
	void Draw(UIContext &dc) override {
		const auto bounds = GetBounds();
		float logoWidth = 0, logoHeight = 0;
		if (dc.Draw()->MeasureImage(ImageID("I_LOGO"), &logoWidth, &logoHeight) && logoWidth > 0 && logoHeight > 0) {
			const float scale = std::min(114.f / logoWidth, 20.f / logoHeight);
			dc.Draw()->DrawImage(ImageID("I_LOGO"), bounds.x + 6, bounds.centerY(), scale, TEXT, ALIGN_LEFT | ALIGN_VCENTER);
		}
		dc.SetFontStyle(dc.GetTheme().uiFontSmall);
		auto di = GetI18NCategory(I18NCat::DIALOG);
		float x = bounds.x2() - 12;
		auto hint = [&](ImageID image, std::string_view key, std::string_view caption) {
			float tw, th;
			dc.MeasureText(dc.GetTheme().uiFontSmall, 1.f, 1.f, caption, &tw, &th);
			x -= tw;
			dc.DrawText(caption, x, bounds.centerY(), DIM, ALIGN_VCENTER);
			x -= 44;
			dc.Draw()->DrawRoundRect(Bounds(x, bounds.centerY() - 15, 34, 30), 6, PANEL);
			if (image.isValid()) {
				float iw, ih;
				if (dc.Draw()->MeasureImage(image, &iw, &ih) && iw > 0 && ih > 0) {
					dc.Draw()->DrawImage(image, x + 17, bounds.centerY(), std::min(20.f / iw, 20.f / ih), TEXT, ALIGN_CENTER);
				}
			} else if (key.empty()) {
				dc.FillRect(UI::Drawable(TEXT), Bounds(x + 14, bounds.centerY() - 9, 6, 18));
				dc.FillRect(UI::Drawable(TEXT), Bounds(x + 8, bounds.centerY() - 3, 18, 6));
			} else {
				dc.DrawText(key, x + 17, bounds.centerY(), TEXT, ALIGN_CENTER);
			}
			x -= 22;
		};
		if (categories_) {
			hint(ImageID::invalid(), "L/R", di->T("Category"));
		}
		hint(ImageID::invalid(), "", di->T("Navigate"));
		hint(ImageID::invalid(), "B", resume_ ? GetI18NCategory(I18NCat::PAUSE)->T("Resume") : di->T("Back"));
		hint(ImageID::invalid(), "A", di->T("Select"));
	}

private:
	bool resume_;
	bool categories_;
};

UI::View *Footer(bool resume, bool categories) { return new FooterView(resume, categories); }

class DeleteStateButton final : public UI::Button {
public:
	DeleteStateButton(std::string_view title, UI::LayoutParams *params) : Button(title, params) {}
	void Draw(UIContext &dc) override {
		const UI::Theme *original = &dc.GetTheme();
		UI::Theme danger = *original;
		danger.itemStyle.fgColor = WARNING;
		danger.itemFocusedStyle.fgColor = WARNING;
		danger.itemDownStyle.fgColor = WARNING;
		dc.SetTheme(&danger);
		Button::Draw(dc);
		dc.SetTheme(original);
	}
};

UI::Button *DeleteButton(std::string_view title, UI::LayoutParams *params) { return new DeleteStateButton(title, params); }

UI::Button *UndoButton(std::string_view title) {
	auto *button = new UI::Button(title, ImageID("I_NAVIGATE_BACK"), new UI::LinearLayoutParams(UI::WRAP_CONTENT, 36));
	button->SetScale(.8f);
	button->SetPadding(8, 4);
	button->SetRoundCorners();
	return button;
}

void CompactMenu(UI::ViewGroup *group) {
	for (int i = 0; i < group->GetNumSubviews(); ++i) {
		UI::View *child = group->GetViewByIndex(i);
		if (auto *choice = dynamic_cast<UI::Choice *>(child)) {
			choice->ReplaceLayoutParams(new UI::LinearLayoutParams(UI::FILL_PARENT, 50));
			choice->SetRoundCorners();
		} else if (auto *children = dynamic_cast<UI::ViewGroup *>(child)) {
			CompactMenu(children);
		}
	}
}

} // namespace SwitchUI
