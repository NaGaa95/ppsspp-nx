// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#include "SDL/SwitchLauncherUtil.h"
#include "Common/SwitchUIColors.h"

#include <cassert>
#include <cstdio>
#include <set>

using namespace SwitchLauncher;

int main() {
	const auto gameMenu = LayoutMenuRows(10, 148, 608, 56, true);
	assert(gameMenu.visible == 10 && gameMenu.height >= 32);
	assert(gameMenu.top + gameMenu.visible * gameMenu.height + 56 <= 608);
	for (int count : {1, 2, 9, 10, 12, 40}) {
		const auto menu = LayoutMenuRows(count, 148, 608, 56, count <= 12);
		assert(menu.visible > 0 && menu.visible <= count);
		assert(menu.top >= 148 && menu.top + menu.height * menu.visible + 56 <= 608);
	}
	for (const auto dimensions : {std::pair{144, 80}, {480, 720}, {1280, 720}}) {
		int uploaded = 0, frames = 0;
		while (uploaded < dimensions.second) {
			const int rows = ArtworkUploadRows(dimensions.first, dimensions.second, uploaded, 512 * 1024);
			assert(rows > 0 && rows <= dimensions.second - uploaded);
			assert((size_t)rows * dimensions.first * 4 <= 512 * 1024);
			uploaded += rows;
			++frames;
		}
		assert(uploaded == dimensions.second);
		assert(frames <= 8);
	}
	assert(ArtworkUploadRows(0, 720, 0, 512 * 1024) == 0);
	assert(ArtworkUploadRows(480, 720, -1, 512 * 1024) == 0);
	assert(ArtworkUploadRows(480, 720, 720, 512 * 1024) == 0);
	assert(ArtworkUploadRows(1280, 720, 0, 1) == 1);
	assert(NormalizeLibraryPath("sdmc:/PSP/GAME/Test/EBOOT.PBP") == NormalizeLibraryPath("/psp/game/test/"));
	assert(NormalizeLibraryPath("/games/Test.ISO") == NormalizeLibraryPath("sdmc:/games/test.iso"));
	assert(NormalizeLibraryPath("usbfs0:/Games/Test.iso") != NormalizeLibraryPath("usbfs1:/Games/Test.iso"));
	assert(NormalizeLibraryPath("usbfs0:/Test/EBOOT.PBP") == NormalizeLibraryPath("usbfs0:/Test"));
	// Different dumps of one game must remain independently launchable.
	assert(NormalizeLibraryPath("/games/US/Test.iso") != NormalizeLibraryPath("/games/EU/Test.iso"));
	std::set<std::string> games;
	for (const char *path : {"sdmc:/PSP/GAME/Test", "/PSP/GAME/Test/EBOOT.PBP", "/games/A.iso", "sdmc:/games/a.iso"}) {
		games.insert(NormalizeLibraryPath(path));
	}
	assert(games.size() == 2);

	assert(LibraryPage(0, 1, 5, 2, 0, false) == 0);
	assert(LibraryPage(3, -1, 5, 2, 25, false) == 3);
	assert(LibraryPage(3, 1, 5, 2, 25, false) == 13);
	assert(LibraryPage(18, 1, 5, 2, 23, false) == 22);
	assert(LibraryPage(22, 1, 5, 2, 23, false) == 22);
	assert(LibraryPage(22, -1, 5, 2, 23, false) == 12);
	assert(LibraryPage(3, 1, 5, 2, 25, true) == 8);
	assert(LibraryPage(23, 1, 5, 2, 25, true) == 24);
	assert(LibraryPage(1, -1, 5, 2, 25, true) == 0);
	assert(LibraryMove(11, 2, 6, 2, 25) == 11);
	assert(LibraryMove(11, 1, 6, 2, 25) == 12);
	assert(LibraryMove(12, -1, 6, 2, 25) == 11);
	assert(LibraryMove(5, 2, 6, 2, 25) == 11);
	assert(LibraryMove(24, 2, 6, 2, 25) == 24);
	for (int columns = 3; columns <= 8; ++columns) {
		for (int rows = 1; rows <= 3; ++rows) {
			for (int count = 1; count <= 50; ++count) {
				for (int selected = 0; selected < count; ++selected) {
					for (int direction : {-2, 2}) {
						const int next = LibraryMove(selected, direction, columns, rows, count);
						assert(next >= 0 && next < count);
						assert(next / (columns * rows) == selected / (columns * rows));
					}
				}
			}
		}
	}
	bool trigger = false;
	assert(!TriggerPressed(12000, trigger));
	assert(TriggerPressed(32767, trigger));

	StickNavigation stick;
	stick.Axis(true, 32767);
	assert(stick.Next(0, false) == 0);
	assert(stick.Next(1000, false) == 0);
	assert(stick.Next(1001, true) == 1);
	stick.Reset();
	stick.Axis(false, 32767);
	assert(stick.Next(0, false) == 2);
	assert(stick.Next(399, false) == 0);
	assert(stick.Next(400, false) == 2);
	for (int now = 401; now < 570; ++now) {
		stick.Axis(false, now & 1 ? 30000 : 26000);
		assert(stick.Next(now, false) == 0);
	}
	assert(stick.Next(570, false) == 2);
	stick.Axis(false, 15000);
	assert(stick.Next(571, false) == 0);
	stick.Axis(false, 0);
	assert(stick.Next(572, false) == 0);
	stick.Axis(false, -32768);
	assert(stick.Next(573, false) == -2);
	assert(SwitchUI::StatusColor(true) == 0xff78dc78);
	assert(SwitchUI::StatusColor(false) == 0xff737deb);
	assert(!TriggerPressed(31000, trigger));
	assert(!TriggerPressed(0, trigger));
	assert(TriggerPressed(32767, trigger));

	assert(WithoutMnemonics("R&eset") == "Reset");
	assert(WithoutMnemonics("&Back") == "Back");
	assert(WithoutMnemonics("Save && load") == "Save & load");
	assert(WithoutMnemonics("Library & storage") == "Library & storage");
	assert(WithoutMnemonics("Ré&initialiser") == "Réinitialiser");

	assert(SliderValue(334.1f, 0, 1000, 1) == 334);
	assert(SliderValue(-10, 0, 1000, 1) == 0);
	assert(SliderValue(1100, 0, 1000, 1) == 1000);
	assert(SliderValue(65535, 0, 65535, 100) == 65535);
	assert(std::abs(SliderValue(.369f, .02f, .98f, .01f) - .37f) < .0001f);

	for (auto section : {"General", "CPU", "Graphics", "Textures", "Performance", "Speed", "Sound", "SystemParam", "Control", "Network"}) {
		const auto &keys = LauncherSettingKeys(section);
		assert(!keys.empty());
		assert(std::set<std::string_view>(keys.begin(), keys.end()).size() == keys.size());
		for (auto key : keys) {
			assert(key != "FileLogging" && key != "CheckForNewVersion" && key != "DiscordRichPresence" && key != "UISound");
			assert(key != "VulkanDevice" && key != "CameraDevice" && key != "ExternalDisplay" && key != "GpuLogProfiler");
			assert(key != "AndroidAudioBackend" && key != "AudioDevice" && key != "MicDevice" && key != "HapticFeedback");
			assert(key != "EnableUPnP");
		}
	}
	assert(LauncherSettingKeys("Graphics")[1] == "InternalResolution");
	assert(LauncherSettingKeys("Debugger").empty());
	puts("Switch launcher: library identity, paging, labels, slider bounds and supported settings passed");
	return 0;
}
