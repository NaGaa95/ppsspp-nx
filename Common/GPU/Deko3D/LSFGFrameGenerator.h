// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

#pragma once

#include <deko3d.hpp>
#include <functional>
#include <memory>
#include <string>

namespace Deko3DLSFG {

struct Config {
	bool performance = true;
	unsigned flowDenominator = 4;
};

struct Services {
	DkDevice device = nullptr;
	uint32_t scratchPerWarp = 0;
	std::function<dk::UniqueMemBlock(uint32_t, uint32_t, const char *)> allocate;
	std::function<void(dk::UniqueMemBlock)> retire;
	std::function<void(const std::string &)> log;
};

class FrameGenerator {
public:
	virtual ~FrameGenerator() = default;
	virtual bool Generate(DkCmdBuf commands, const DkImage &source, uint32_t width, uint32_t height) = 0;
	virtual const DkImage *GeneratedImage() const = 0;
	virtual bool Failed() const = 0;
	virtual void Reset() = 0;
};

// The owner retires all allocations through its normal GPU fence mechanism.
std::unique_ptr<FrameGenerator> CreateFrameGenerator(Services services, const Config &config, const std::string &directory, uint32_t width,
													 uint32_t height);

} // namespace Deko3DLSFG
