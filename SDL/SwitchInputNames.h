// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include "Common/Input/InputState.h"

namespace SwitchLauncher {

// Names match SDLJoystick's Switch event mapping and Nintendo button positions.
inline const char *GamepadInputName(const InputMapping &mapping) {
	if (mapping.deviceId == DEVICE_ID_KEYBOARD || mapping.deviceId == DEVICE_ID_MOUSE) {
		return nullptr;
	}
	if (mapping.IsAxis()) {
		int direction = 0;
		switch (mapping.Axis(&direction)) {
		case JOYSTICK_AXIS_X:
			return direction < 0 ? "Left stick Left" : "Left stick Right";
		case JOYSTICK_AXIS_Y:
			return direction < 0 ? "Left stick Up" : "Left stick Down";
		case JOYSTICK_AXIS_Z:
			return direction < 0 ? "Right stick Left" : "Right stick Right";
		case JOYSTICK_AXIS_RZ:
			return direction < 0 ? "Right stick Up" : "Right stick Down";
		case JOYSTICK_AXIS_LTRIGGER:
			return "ZL";
		case JOYSTICK_AXIS_RTRIGGER:
			return "ZR";
		case JOYSTICK_AXIS_HAT_X:
			return direction < 0 ? "D-pad Left" : "D-pad Right";
		case JOYSTICK_AXIS_HAT_Y:
			return direction < 0 ? "D-pad Up" : "D-pad Down";
		default:
			return nullptr;
		}
	}
	if (mapping.deviceId == DEVICE_ID_SWITCH_RIGHT_STICK) {
		switch (mapping.keyCode) {
		case NKCODE_DPAD_UP:
			return "Right stick Up";
		case NKCODE_DPAD_DOWN:
			return "Right stick Down";
		case NKCODE_DPAD_LEFT:
			return "Right stick Left";
		case NKCODE_DPAD_RIGHT:
			return "Right stick Right";
		default:
			return nullptr;
		}
	}
	switch (mapping.keyCode) {
	case NKCODE_BUTTON_1:
	case NKCODE_BUTTON_Y:
		return "X";
	case NKCODE_BUTTON_2:
	case NKCODE_BUTTON_A:
		return "B";
	case NKCODE_BUTTON_3:
	case NKCODE_BUTTON_B:
		return "A";
	case NKCODE_BUTTON_4:
	case NKCODE_BUTTON_X:
		return "Y";
	case NKCODE_BUTTON_5:
	case NKCODE_BUTTON_R1:
		return "R";
	case NKCODE_BUTTON_6:
	case NKCODE_BUTTON_L1:
		return "L";
	case NKCODE_BUTTON_L2:
		return "ZL";
	case NKCODE_BUTTON_R2:
		return "ZR";
	case NKCODE_BUTTON_9:
	case NKCODE_BUTTON_SELECT:
		return "Minus";
	case NKCODE_BUTTON_10:
	case NKCODE_BUTTON_START:
		return "Plus";
	case NKCODE_BUTTON_THUMBL:
		return "Left stick click";
	case NKCODE_BUTTON_THUMBR:
		return "Right stick click";
	case NKCODE_DPAD_UP:
		return "D-pad Up";
	case NKCODE_DPAD_DOWN:
		return "D-pad Down";
	case NKCODE_DPAD_LEFT:
		return "D-pad Left";
	case NKCODE_DPAD_RIGHT:
		return "D-pad Right";
	case NKCODE_BACK:
		return "HOME";
	default:
		return nullptr;
	}
}

} // namespace SwitchLauncher
