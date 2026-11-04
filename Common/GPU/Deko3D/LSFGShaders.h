// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

#pragma once

#include <atomic>
#include <string>

namespace Deko3DLSFG {

struct PreparedShaders {
	std::string directory;
	std::string error;
	bool cancelled = false;
};

PreparedShaders PrepareShaders(bool performance, const std::atomic<bool> &cancel,
							   const std::string &dll = "/switch/ppsspp-nx/lsfg/Lossless.dll",
							   const std::string &cacheRoot = "/switch/ppsspp-nx/cache/deko3d/lsfg-1");

} // namespace Deko3DLSFG
