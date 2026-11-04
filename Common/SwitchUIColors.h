// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <cstdint>

namespace SwitchUI {

constexpr uint32_t Color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
	return r | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
}

constexpr uint32_t TEXT = Color(235, 248, 255);
constexpr uint32_t DIM = Color(143, 192, 216);
constexpr uint32_t VALUE = Color(194, 239, 255);
constexpr uint32_t ACCENT = Color(61, 183, 235);
constexpr uint32_t PANEL = Color(16, 23, 39, 235);
constexpr uint32_t FOCUS = Color(28, 69, 92);
constexpr uint32_t GOOD = Color(120, 220, 120);
constexpr uint32_t WARNING = Color(235, 125, 115);

constexpr uint32_t StatusColor(bool ready) { return ready ? GOOD : WARNING; }

} // namespace SwitchUI
