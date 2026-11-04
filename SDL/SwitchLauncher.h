// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <string>
#include <optional>

class Screen;
class Path;

namespace SwitchLauncher {

enum class Action { Launch, Restart, Quit, Unavailable };
struct Result {
	Action action = Action::Unavailable;
	std::string path;
	std::string mountIso;
	std::optional<bool> umdAutoboot;
};

// Runs before the emulator owns a graphics context. All SDL textures are released
// before returning, so Mesa and the native renderer never share a live window.
Result Run();
void RecordLaunch(const Path &path);

} // namespace SwitchLauncher
