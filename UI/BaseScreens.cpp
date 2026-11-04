#include "UI/BaseScreens.h"
#include "Core/Config.h"
#if defined(PPSSPP_SWITCH_SDL_LAUNCHER)
#include "UI/SwitchUIStyle.h"
#endif

bool UIBaseScreen::UseImmersiveMode() const {
	DisplayLayoutConfig &config = g_Config.GetDisplayLayoutConfig(GetDeviceOrientation());
	bool portrait = GetDeviceOrientation() == DeviceOrientation::Portrait;
	if (!portrait && config.bImmersiveMode) {
		return true;
	}
	return false;
}

ViewLayoutMode UIBaseDialogScreen::LayoutMode() const {
	return ViewLayoutMode::IgnoreBottomInset;
}

bool UIBaseDialogScreen::UseImmersiveMode() const {
	DisplayLayoutConfig &config = g_Config.GetDisplayLayoutConfig(GetDeviceOrientation());
	bool portrait = GetDeviceOrientation() == DeviceOrientation::Portrait;
	if (!portrait && config.bImmersiveMode) {
		return true;
	}
	return false;
}

void UIBaseDialogScreen::DrawBackground(UIContext &ui) {
#if defined(PPSSPP_SWITCH_SDL_LAUNCHER)
	if (!wantBrightBackground()) {
		SwitchUI::DrawBackdrop(ui);
	}
#endif
}
