// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <string_view>
#include <functional>

class Path;
class UIContext;
namespace UI {
class View;
class ViewGroup;
struct Theme;
class Button;
class LayoutParams;
} // namespace UI

namespace SwitchUI {
void ApplyTheme(UI::Theme &theme);
void DrawBackdrop(UIContext &dc);
UI::View *Header(const Path &gamePath, std::string_view title, std::string_view subtitle, bool gameStatus = false,
				 std::function<void()> info = {});
UI::View *Footer(bool resume = false, bool categories = false);
UI::Button *DeleteButton(std::string_view title, UI::LayoutParams *params);
UI::Button *UndoButton(std::string_view title);
void CompactMenu(UI::ViewGroup *group);
} // namespace SwitchUI
