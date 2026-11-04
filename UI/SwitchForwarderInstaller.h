// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

void SwitchForwarder_SetExecutablePath(std::string path);
bool SwitchForwarder_Create(const std::string &gamePath, const std::string &name, const std::string &author,
							const std::vector<uint8_t> &iconData, std::string *error);
