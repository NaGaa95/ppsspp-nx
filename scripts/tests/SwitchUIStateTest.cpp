// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

// Reuse the unit executable's platform services and real core/library link graph.
#define main ExistingUnitMain
#include "unittest/UnitTest.cpp"
#undef main
#include "Common/UI/Screen.h"
#include "Core/HLE/sceUtility.h"
#include "SDL/SwitchLauncherUtil.h"
#include <cassert>

struct RoutingScreens {
	InputMode mode = InputMode::None;
	InputMode PassInputToMapper() { return mode; }
} routingScreens;
RoutingScreens *g_screenManager = &routingScreens;
bool imguiInited_ = true;
namespace RoutingImGui {
struct IO { bool WantCaptureKeyboard = false, WantCaptureMouse = false; } io;
IO &GetIO() { return io; }
}
void RouteKey(const KeyInput &key) {
	// KEY_ROUTE
}
void RouteAxis(const AxisInput *axes, size_t count) {
	// AXIS_ROUTE
}
namespace MenuProbe {
using namespace KeyMap;
// MENU_HELPERS
#undef PPSSPP_PLATFORM
#define PPSSPP_PLATFORM(name) PLATFORM_##name
#define PLATFORM_SWITCH 1
#define PLATFORM_ANDROID 0
void UpdateMenuKeys() // MENU_KEYS
}

std::string configPath;
const char *CONFIG_PATH = nullptr;
unsigned renameCall = 0, failRename = 0;
bool failCommitAndRollback = false;
namespace HorizonFiles {
bool Rename(const Path &source, const Path &target) {
	++renameCall;
	if (renameCall == failRename || (failCommitAndRollback && renameCall >= 2) || File::Exists(target)) { return false; }
	return File::Rename(source, target);
}
}
bool LoadLauncherConfig(IniFile &config) // LOAD_CONFIG
bool WriteLauncherConfig(IniFile &config) // WRITE_CONFIG
struct LauncherProbe {
	IniFile ini_;
	Section *appearance_;
	bool classic_ = false, animations_ = true, titles_ = true, showRegions_ = true, updateAtBoot_ = true, badges_ = true;
	// COLUMN_DEFAULT
	int rows_ = 2, view_ = 1;
	std::set<std::string> favorites_;
	std::vector<std::string> recentKeys_;
	LauncherProbe() {
		LoadLauncherConfig(ini_);
		appearance_ = ini_.GetOrCreateSection("Launcher");
		// LOAD_APPEARANCE
		for (auto &[key, value] : ini_.GetOrCreateSection("Favorites")->ToMap()) {
			if (value == "True") { favorites_.insert(key); }
		}
	}
	void SaveAppearance() // SAVE_APPEARANCE
};

struct GameListener : ControlListener {
	uint32_t buttons = 0;
	bool onTop = true;
	bool fastForward = false;
	float x = 0, y = 0;
	void UpdatePSPButtons(uint32_t down, uint32_t up) override {
		buttons = (buttons & ~up) | (onTop ? down : 0);
	}
	void OnVKey(VirtKey key, bool down) override {
		if (key == VIRTKEY_FASTFORWARD) { fastForward = down; }
	}
	void SetPSPAnalog(int, int, float nx, float ny) override { x = onTop ? nx : 0; y = onTop ? ny : 0; }
};
bool Has(const std::vector<InputMapping> &keys, InputDeviceID device, int code) {
	return std::find(keys.begin(), keys.end(), InputMapping(device, code)) != keys.end();
}
// WIDGET_TESTS
int main(int argc, char **argv) {
	assert(argc == 2);
	SetupCRT(true); TimeInit(); g_logManager.SetAllLogEnable(false);
	g_Config.bStrictComboOrder = false;
	g_Config.fAnalogDeadzone = .15f; g_Config.fAnalogInverseDeadzone = 0; g_Config.fAnalogSensitivity = 1;
	g_Config.fAnalogTriggerThreshold = g_Config.fAnalogStickThreshold = .75f;
	g_Config.iAnalogResponseCurve = g_Config.iAnalogDeadzoneShape = 0;
	g_Config.bAnalogIsCircular = false;
	g_Config.fAnalogAxialDeadzone = 0;
	GameListener game;
	g_controlMapper.AddListener(&game);
	const auto inGame = InputMode::Keyboard | InputMode::Mouse | InputMode::Other | InputMode::ImDebuggerToggle;
	const uint32_t buttons[] = {CTRL_CROSS, CTRL_CIRCLE, CTRL_TRIANGLE, CTRL_SQUARE, CTRL_LTRIGGER, CTRL_RTRIGGER, CTRL_UP, CTRL_DOWN, CTRL_LEFT, CTRL_RIGHT, CTRL_START, CTRL_SELECT};
	for (int physical : {NKCODE_BUTTON_6, NKCODE_BUTTON_1, NKCODE_BUTTON_2, NKCODE_BUTTON_3, NKCODE_BUTTON_4}) {
		for (uint32_t output : buttons) {
			for (auto menu : {InputMode::ImDebuggerToggle, InputMode::None, InputMode::AllKeys}) {
				KeyMap::ClearAllMappings();
				KeyMap::SetInputMapping(output, KeyMap::MultiInputMapping(InputMapping(DEVICE_ID_PAD_0, physical)), true);
				game.onTop = true; routingScreens.mode = inGame;
				RouteKey(KeyInput(DEVICE_ID_PAD_0, (InputKeyCode)physical, KeyInputFlags::DOWN));
				assert(game.buttons & output);
				game.onTop = false; routingScreens.mode = menu;
				RouteKey(KeyInput(DEVICE_ID_PAD_0, (InputKeyCode)physical, KeyInputFlags::UP));
				assert(!(game.buttons & output));
				game.onTop = true; routingScreens.mode = inGame;
				assert(game.buttons == 0);
				RouteKey(KeyInput(DEVICE_ID_PAD_0, (InputKeyCode)physical, KeyInputFlags::DOWN));
				assert(game.buttons & output);
				RouteKey(KeyInput(DEVICE_ID_PAD_0, (InputKeyCode)physical, KeyInputFlags::UP));
				assert(game.buttons == 0);
			}
		}
	}
	// Menu presses must not enter gameplay or start a hotkey.
	KeyMap::ClearAllMappings();
	KeyMap::SetInputMapping(VIRTKEY_FASTFORWARD, KeyMap::MultiInputMapping(InputMapping(DEVICE_ID_PAD_0, NKCODE_BUTTON_6)), true);
	game.onTop = true; routingScreens.mode = inGame;
	RouteKey(KeyInput(DEVICE_ID_PAD_0, NKCODE_BUTTON_6, KeyInputFlags::DOWN)); assert(game.fastForward);
	game.onTop = false; routingScreens.mode = InputMode::ImDebuggerToggle;
	RouteKey(KeyInput(DEVICE_ID_PAD_0, NKCODE_BUTTON_6, KeyInputFlags::UP)); assert(!game.fastForward);
	RouteKey(KeyInput(DEVICE_ID_PAD_0, NKCODE_BUTTON_6, KeyInputFlags::DOWN)); assert(!game.fastForward);
	RouteKey(KeyInput(DEVICE_ID_PAD_0, NKCODE_BUTTON_6, KeyInputFlags::UP));
	// Recenter inside a nested settings screen, where new axis presses are filtered.
	KeyMap::ClearAllMappings();
	KeyMap::SetInputMapping(VIRTKEY_AXIS_X_MAX, KeyMap::MultiInputMapping(InputMapping(DEVICE_ID_PAD_0, JOYSTICK_AXIS_X, 1)), true);
	game.onTop = true; routingScreens.mode = inGame;
	AxisInput axis{DEVICE_ID_PAD_0, JOYSTICK_AXIS_X, 1}; RouteAxis(&axis, 1); assert(game.x > .9f);
	game.onTop = false; routingScreens.mode = InputMode::None; axis.value = 0; RouteAxis(&axis, 1); assert(game.x == 0);
	g_controlMapper.RemoveListener(&game);
	puts("Input checks passed: 180 mapped hold/pause/release/resume cases, next presses, filtered presses, fast-forward release, nested axis release.");
	// Menu A/B stays physical when the game remaps those buttons or changes PSP confirm preference.
	for (int preference : {PSP_SYSTEMPARAM_BUTTON_CROSS, PSP_SYSTEMPARAM_BUTTON_CIRCLE}) {
		g_Config.iButtonPreference = preference;
		KeyMap::ClearAllMappings();
		KeyMap::SetInputMapping(CTRL_CROSS, KeyMap::MultiInputMapping(InputMapping(DEVICE_ID_PAD_0, NKCODE_BUTTON_6)), true);
		KeyMap::SetInputMapping(CTRL_CIRCLE, KeyMap::MultiInputMapping(InputMapping(DEVICE_ID_PAD_0, NKCODE_BUTTON_1)), true);
		MenuProbe::UpdateMenuKeys();
		assert(Has(confirmKeys, DEVICE_ID_PAD_0, NKCODE_BUTTON_3) && !Has(cancelKeys, DEVICE_ID_PAD_0, NKCODE_BUTTON_3));
		assert(Has(cancelKeys, DEVICE_ID_PAD_0, NKCODE_BUTTON_2) && !Has(confirmKeys, DEVICE_ID_PAD_0, NKCODE_BUTTON_2));
		assert(!Has(confirmKeys, DEVICE_ID_PAD_0, NKCODE_BUTTON_6) && !Has(cancelKeys, DEVICE_ID_PAD_0, NKCODE_BUTTON_1));
		assert(Has(confirmKeys, DEVICE_ID_KEYBOARD, NKCODE_ENTER) && Has(cancelKeys, DEVICE_ID_KEYBOARD, NKCODE_ESCAPE));
		assert(Has(tabLeftKeys, DEVICE_ID_PAD_0, NKCODE_BUTTON_6) && Has(tabRightKeys, DEVICE_ID_PAD_0, NKCODE_BUTTON_5));
		std::vector<int> mapped; KeyMap::InputMappingToPspButton(InputMapping(DEVICE_ID_PAD_0, NKCODE_BUTTON_6), &mapped);
		assert(std::find(mapped.begin(), mapped.end(), CTRL_CROSS) != mapped.end());
	}
	puts("Switch menu keys passed: physical A confirm/B back, independent PSP preferences/mappings, keyboard and shoulders.");
	TestWidgets();
	configPath = std::string(argv[1]) + "/launcher.ini"; CONFIG_PATH = configPath.c_str();
	assert(LauncherProbe().columns_ == 6);
	{
		LauncherProbe p; p.columns_ = 5; p.favorites_.insert("favorite");
		p.ini_.GetOrCreateSection("Aliases")->Set("game", "Custom title");
		p.SaveAppearance();
	}
	assert(LauncherProbe().columns_ == 5);
	{
		LauncherProbe p; p.columns_ = 6; p.classic_ = true; p.animations_ = false; p.titles_ = false; p.rows_ = 3;
		p.SaveAppearance();
	}
	{
		LauncherProbe p; assert(p.columns_ == 6 && p.rows_ == 3 && p.classic_ && !p.animations_ && !p.titles_);
		std::string alias; assert(p.ini_.GetOrCreateSection("Aliases")->Get("game", &alias) && alias == "Custom title");
		assert(p.favorites_.count("favorite"));
	}
	for (unsigned failure : {1u, 2u}) {
		LauncherProbe p; p.columns_ = 8; p.appearance_->Set("Columns", p.columns_);
		renameCall = 0; failRename = failure; assert(!WriteLauncherConfig(p.ini_)); failRename = 0;
		assert(LauncherProbe().columns_ == 6);
	}
	{
		LauncherProbe p; p.appearance_->Set("Columns", 8);
		renameCall = 0; failCommitAndRollback = true; assert(!WriteLauncherConfig(p.ini_)); failCommitAndRollback = false;
		assert(LauncherProbe().columns_ == 6);
		LauncherProbe restored; restored.SaveAppearance(); assert(LauncherProbe().columns_ == 6);
	}
	IniFile full; full.GetOrCreateSection("Test")->Set("Data", std::string(1024, 'x'));
	assert(!full.Save(Path("/dev/full")));
	puts("Launcher persistence passed: six-cover default, save/restart, existing-target replacement, other preferences/aliases/favorites, failed rename/rollback, short writes.");
	KeyMap::ClearAllMappings();
}
