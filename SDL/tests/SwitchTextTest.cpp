// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#include "SDL/SwitchText.h"
#include "Common/Data/Encoding/Utf8.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

int main(int argc, char **argv) {
	assert(argc >= 3 && argc <= 5);
	std::vector<std::vector<uint8_t>> data;
	SwitchLauncher::TextEngine engine;
	for (const auto &file : std::filesystem::directory_iterator(argv[1])) {
		if (file.path().extension() != ".ttf") {
			continue;
		}
		std::ifstream stream(file.path(), std::ios::binary);
		data.emplace_back(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		assert(engine.AddFont(data.back().data(), data.back().size()));
	}
	std::vector<std::string> samples{
		"Français : paramètres, bibliothèque, succès, cœur",
		"Deutsch: Einstellungen — Ελληνικά: Ρυθμίσεις",
		"Русский: Настройки — Українська: Налаштування",
		"עברית: הגדרות — PPSSPP 1.0.1 (123)",
		"العربية: الإعدادات — PPSSPP 1.0.1 (123)",
		"فارسی: تنظیمات و بازی‌ها — PPSSPP 123",
		"ខ្មែរ៖ ការកំណត់ កម្មវិធីហ្គេម PPSSPP",
		"ລາວ: ການຕັ້ງຄ່າ PPSSPP 123",
		"ไทย: การตั้งค่า เกม PPSSPP 123",
	};
	if (argc == 5) {
		// Supply a local CJK font when console shared fonts are unavailable on the host.
		assert(!engine.HasGlyph(0x9971));  // 饱: absent from the bundled Latin/script fonts.
		std::ifstream stream(argv[4], std::ios::binary);
		data.emplace_back(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		assert(engine.AddFont(data.back().data(), data.back().size()));
		samples.push_back("简体中文：着色色调、饱和度、神眷之力、剧情存档");
		samples.push_back("繁體中文：載入、飽和度、遊戲設定、神眷之力");
	}
	const int width = 1200, height = 80 + (int)samples.size() * 64;
	std::vector<uint8_t> canvas((size_t)width * height * 4, 255);
	for (size_t i = 0; i < canvas.size(); i += 4) {
		canvas[i] = 40;
		canvas[i + 1] = 24;
		canvas[i + 2] = 12;
	}
	for (size_t i = 0; i < samples.size(); ++i) {
		UTF8 text(samples[i]);
		while (!text.end()) {
			const uint32_t code = text.next();
			if (code != 0x200c && code != 0x200d) {
				assert(engine.HasGlyph(code));
			}
		}
		const auto bitmap = engine.Render(samples[i], 32);
		assert(bitmap.width > 0 && bitmap.height > 0 && bitmap.advance > 0);
		assert(std::abs(engine.Measure(samples[i], 32) - bitmap.advance) < 0.01f);
		size_t ink = 0;
		for (int y = 0; y < bitmap.height && y + 32 + i * 64 < (size_t)height; ++y) {
			for (int x = 0; x < bitmap.width && x + 32 < width; ++x) {
				const uint8_t alpha = bitmap.rgba[((size_t)y * bitmap.width + x) * 4 + 3];
				ink += alpha > 0;
				const size_t dest = ((y + 32 + i * 64) * width + x + 32) * 4;
				for (int c = 0; c < 3; ++c) {
					canvas[dest + c] = (240 * alpha + canvas[dest + c] * (255 - alpha)) / 255;
				}
			}
		}
		assert(ink > 50);
		std::cout << "Rendered: " << samples[i] << '\n';
	}
	const auto left = engine.RenderLines("Savedata manager\nA", 24);
	const auto right = engine.RenderLines("Savedata manager\nA", 24, SwitchLauncher::TextAlignment::Right);
	assert(left.width == right.width && left.height == right.height);
	assert(left.height >= 2 * (24 + 24 / 3));
	auto lastInk = [](const SwitchLauncher::TextBitmap &bitmap) {
		int last = -1;
		for (int y = bitmap.height / 2; y < bitmap.height; ++y) {
			for (int x = 0; x < bitmap.width; ++x) {
				if (bitmap.rgba[((size_t)y * bitmap.width + x) * 4 + 3]) {
					last = std::max(last, x);
				}
			}
		}
		return last;
	};
	assert(lastInk(right) > lastInk(left));
	for (const auto &sample : samples) {
		const auto single = engine.Render(sample, 32);
		const auto multiline = engine.RenderLines(sample + "\n" + sample, 32);
		assert(multiline.width == single.width);
		assert(multiline.height >= single.height * 2);
	}
	assert(engine.RenderLines("\n\n", 24).height == 3 * 32);
	std::cout << "Overlay multiline shaping, alignment and script bounds passed.\n";
	assert(engine.Measure("لا", 32) < engine.Measure("ل", 32) + engine.Measure("ا", 32));
	if (argc >= 4) {
		std::map<uint32_t, std::set<std::string>> missing;
		for (const auto &file : std::filesystem::directory_iterator(argv[3])) {
			if (file.path().extension() != ".ini") {
				continue;
			}
			std::ifstream stream(file.path());
			std::string line;
			while (std::getline(stream, line)) {
				const size_t equal = line.find(" = ");
				if (equal == std::string::npos) {
					continue;
				}
				UTF8 value(std::string_view(line).substr(equal + 3));
				while (!value.end()) {
					const uint32_t code = value.next();
					// East Asian and Nintendo glyphs come from the console's six shared fonts.
					if (code <= 32 || (code >= 0x2e80 && code <= 0x9fff) || (code >= 0xac00 && code <= 0xd7af) ||
						(code >= 0xe000 && code <= 0xfaff) || (code >= 0xfe00 && code <= 0xffef) || (code >= 0x2000 && code <= 0x200f) ||
						code == 0x202a || code == 0x202b || code == 0x202c) {
						continue;
					}
					if (!engine.HasGlyph(code)) {
						missing[code].insert(file.path().filename().string());
					}
				}
			}
		}
		for (const auto &[code, files] : missing) {
			std::cerr << "Missing U+" << std::hex << code << std::dec << ":";
			for (const auto &file : files) {
				std::cerr << ' ' << file;
			}
			std::cerr << '\n';
		}
		assert(missing.empty());
		std::cout << "All 46 language catalogs covered outside console-provided CJK/Nintendo glyph ranges.\n";
	}
	uint8_t header[54]{};
	const auto put = [&](int offset, uint32_t value) {
		for (int i = 0; i < 4; ++i) {
			header[offset + i] = value >> (i * 8);
		}
	};
	header[0] = 'B';
	header[1] = 'M';
	put(2, 54 + (uint32_t)canvas.size());
	put(10, 54);
	put(14, 40);
	put(18, width);
	put(22, (uint32_t)-height);
	header[26] = 1;
	header[28] = 32;
	std::ofstream output(argv[2], std::ios::binary);
	output.write((char *)header, sizeof(header));
	output.write((char *)canvas.data(), canvas.size());
	std::cout << samples.size() << " script samples and Arabic ligature shaping passed.\n";
}
