// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

// Production UI method bodies are inserted by test-switch-ui-state.py.
namespace PopupProbe {
struct ImageID {
	std::string name;
	ImageID() = default;
	ImageID(const char *n) : name(n) {}
};
namespace UI {
struct EventParams { void *v = nullptr; int a = 0, b = 0; };
enum class FocusFlags { CAUSE_SCREEN_CHANGE };
}
using UI::FocusFlags;
template<class T> void SetFocusedView(T *, FocusFlags) {}
std::string_view ChopTitle(std::string_view title) { return title; }
struct ListPopupScreen {
	std::vector<std::string> choices;
	int selected, fallback = -99;
	std::function<void(int)> callback;
	std::set<int> hidden;
	std::map<int, ImageID> icons;
	ListPopupScreen(std::string_view, const std::vector<std::string> &c, int s, std::function<void(int)> cb)
		: choices(c), selected(s), callback(std::move(cb)) {}
	void SetHiddenChoices(const std::set<int> &h) { hidden = h; }
	void SetChoiceIcons(const std::map<int, ImageID> &i) { icons = i; }
	void SetDefault(int d) { fallback = d; }
	void SetPopupOrigin(void *) {}
};
struct ScreenManager {
	std::unique_ptr<ListPopupScreen> popup;
	void push(ListPopupScreen *p) { popup.reset(p); }
};
struct PopupMultiChoice {
	int *value_, minVal_ = 0, numChoices_ = 7;
	int firstChoice_ = -1, default_ = -99;
	bool callbackExecuted_ = false, restoreFocus_ = false;
	std::function<void(PopupMultiChoice *)> preOpenCallback_;
	std::string text_ = "Backend";
	std::set<int> hidden_;
	std::map<int, ImageID> icons_;
	ScreenManager manager;
	ScreenManager *screenManager_ = &manager;
	struct Event {
		int chosen = -1, count = 0;
		void Trigger(UI::EventParams &e) { chosen = e.a; ++count; }
	} OnChoice;
	const std::vector<std::string> names{"OpenGL", "Direct3D 9", "Direct3D 11", "Vulkan", "Metal", "Zink", "Deko3D"};
	bool HasFocus() { return true; }
	std::string_view TranslateChoice(int i) { return names.at(i); }
	bool PostChoiceCallback(int) { return true; }
	void UpdateText() {}
	void HandleClick(UI::EventParams &e) // POPUP_CLICK
	void ChoiceCallback(int num) // POPUP_CHOICE
};
void Test() {
	for (int minimum : {0, 10}) {
		for (int choice = 0; choice < 7; ++choice) {
			int value = minimum + choice;
			PopupMultiChoice p;
			p.value_ = &value; p.minVal_ = minimum; p.firstChoice_ = 6; p.default_ = 6;
			p.hidden_ = {1, 2, 4}; p.icons_[6] = ImageID("Deko icon");
			unsigned prepared = 0;
			p.preOpenCallback_ = [&](auto *) { ++prepared; };
			UI::EventParams event;
			p.HandleClick(event);
			const auto &popup = *p.manager.popup;
			assert(prepared == 1 && popup.choices.front() == "Deko3D" && popup.fallback == 0);
			assert(popup.choices[popup.selected] == p.names[choice] && popup.icons.at(0).name == "Deko icon");
			assert(popup.hidden == std::set<int>({2, 3, 5}));
			popup.callback(0); assert(value == minimum + 6 && p.OnChoice.chosen == 6);
			popup.callback(1); assert(value == minimum && p.OnChoice.chosen == 0);
			popup.callback(-1); assert(value == minimum && p.OnChoice.count == 2);
			p.HandleClick(event); assert(prepared == 1);
		}
	}
	int value = 3;
	PopupMultiChoice p; p.value_ = &value;
	UI::EventParams event; p.HandleClick(event);
	assert(p.manager.popup->choices == p.names && p.manager.popup->selected == 3);
}
}

namespace FooterProbe {
struct Bounds {
	float x, y, w, h;
	Bounds(float nx, float ny, float nw, float nh) : x(nx), y(ny), w(nw), h(nh) {}
	float x2() const { return x + w; }
	float centerY() const { return y + h / 2; }
};
namespace UI { struct Drawable { explicit Drawable(uint32_t) {} }; }
struct ImageID {
	std::string name;
	ImageID(const char *n) : name(n) {}
	static ImageID invalid() { return ImageID(""); }
	bool isValid() const { return !name.empty(); }
};
constexpr uint32_t TEXT = 1, DIM = 2, PANEL = 3;
constexpr int ALIGN_LEFT = 1, ALIGN_VCENTER = 2, ALIGN_CENTER = 3;
struct DrawBuffer {
	std::vector<std::string> images;
	bool MeasureImage(ImageID image, float *, float *) { images.push_back(image.name); return false; }
	void DrawImage(ImageID, float, float, float, uint32_t, int) {}
	void DrawRoundRect(Bounds, int, uint32_t) {}
};
struct UIContext {
	struct Theme { int uiFontSmall = 0; } theme;
	DrawBuffer draw;
	std::vector<std::string> labels;
	Theme &GetTheme() { return theme; }
	DrawBuffer *Draw() { return &draw; }
	void MeasureText(int, float, float, std::string_view s, float *w, float *h) { *w = s.size() * 9; *h = 18; }
	void DrawText(int, std::string_view s, float x, float, uint32_t, int) { assert(x >= 0 && x <= 1000); labels.emplace_back(s); }
	void FillRect(UI::Drawable, Bounds) {}
};
struct Category { std::string_view T(std::string_view s) { return s; } } category;
Category *FooterCategory(I18NCat) { return &category; }
struct FooterView {
	bool resume_, categories_;
	Bounds GetBounds() { return {0, 0, 1000, 42}; }
	void Draw(UIContext &dc) // FOOTER_DRAW
};
void Test() {
	for (bool resume : {false, true}) {
		for (bool categories : {false, true}) {
			UIContext dc; FooterView{resume, categories}.Draw(dc);
			assert(std::find(dc.labels.begin(), dc.labels.end(), "A") != dc.labels.end());
			assert(std::find(dc.labels.begin(), dc.labels.end(), "B") != dc.labels.end());
			assert(dc.draw.images == std::vector<std::string>{"I_LOGO"});
		}
	}
}
}

void TestWidgets() {
	PopupProbe::Test(); FooterProbe::Test();
	std::vector<int> values{0, 3, 5, 6};
	std::vector<std::string> choices{"OpenGL", "Vulkan", "Zink", "Deko3D"};
	int value = 3, current = 1;
	// SDL_BACKEND_ORDER
	assert(values == std::vector<int>({6, 0, 3, 5}) && choices[0] == "Deko3D" && values[current] == value);
	puts("UI widget checks passed: A/B footer, Deko-first choices, unchanged stored IDs, selection/default/icons/hidden choices, cancellation, non-Switch order.");
}
