#pragma once
#include <SDL3/SDL.h>
#include <map>
#include <vector>

#include "Common/Input/InputState.h"
#include "Common/Input/KeyCodes.h"
#include "Common/Net/Resolve.h"

class SDLJoystick{
public:
	SDLJoystick(bool init_SDL = false);
	~SDLJoystick();

	void registerEventHandler();
	void ProcessInput(const SDL_Event &event);
#if PPSSPP_PLATFORM(SWITCH)
	void releaseRightStickFaceButtons();
#endif

private:
	void setUpController(SDL_JoystickID deviceID);
	void setUpControllers();
	InputKeyCode getKeycodeForButton(SDL_GamepadButton button);
	int getDeviceIndex(int instanceId);
	void releaseAllKeys();
#if PPSSPP_PLATFORM(SWITCH)
	void updateRightStickFaceButton(InputKeyCode keyCode, bool down, bool *pressed);
#endif

	bool registeredAsEventHandler;
	std::vector<SDL_Gamepad *> controllers;
	std::map<int, int> controllerDeviceMap;

	// Deduplicate axis events. Pair is device, axis.
	std::map<std::pair<InputDeviceID, InputAxis>, float> prevAxisValue_;
#if PPSSPP_PLATFORM(SWITCH)
	bool rightStickUp_ = false;
	bool rightStickDown_ = false;
	bool rightStickLeft_ = false;
	bool rightStickRight_ = false;
#endif
};

extern SDLJoystick *joystick;
