// Copyright (c) 2012- PPSSPP Project.
// Adapted from Cemu-NX's native LSFG shader preparation (GPL-2.0-or-later).
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2 or later.

#include "Common/GPU/Deko3D/LSFGShaders.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include <lsfg/common/cache_format.hpp>
#include <lsfg/common/cache_store.hpp>
#include <lsfg/common/prepare.hpp>
#include <lsfg/common/version.hpp>

namespace Deko3DLSFG {

PreparedShaders PrepareShaders(bool performance, const std::atomic<bool> &cancel, const std::string &dll, const std::string &cacheRoot) {
	PreparedShaders result;
	try {
		if (cancel) {
			result.cancelled = true;
			return result;
		}
		std::ifstream file(dll, std::ios::binary | std::ios::ate);
		const auto size = file ? file.tellg() : std::streampos(-1);
		if (size <= 0 || size > 512 * 1024 * 1024) {
			result.error = "Lossless.dll could not be read";
			return result;
		}
		std::vector<uint8_t> image((size_t)size);
		file.seekg(0);
		if (!file.read((char *)image.data(), image.size())) {
			result.error = "Lossless.dll could not be read";
			return result;
		}
		lsfg::prepare::Options options;
		options.precision = lsfg::shaders::Precision::high;
		options.graph.performance = performance;
		const auto digest = lsfg::sha256(image);
		const auto key = lsfg::cache::cache_key({image, lsfg::cache::extractor_version, lsfg::version::spirv_cross_revision,
												 lsfg::version::uam_revision, options.translation.glsl_version,
												 lsfg::cache::pack_options(lsfg::cache::Precision::high, options.graph),
												 lsfg::cache::backend_abi_version});
		result.directory = lsfg::cache::directory_for(cacheRoot, key);
		lsfg::cache::Loaded cached;
		if (lsfg::succeeded(lsfg::cache::read(result.directory, cached)) && cached.header.dll_hash == digest &&
			cached.header.dll_size == image.size() && cached.header.shader_precision == (uint32_t)lsfg::cache::Precision::high &&
			lsfg::cache::pack_options(lsfg::cache::Precision::high, lsfg::cache::configuration(cached.header)) ==
				lsfg::cache::pack_options(lsfg::cache::Precision::high, options.graph)) {
			result.cancelled = cancel;
			return result;
		}
		lsfg::prepare::Result prepared;
		const auto status = lsfg::prepare::run(image, options, prepared, [&](const auto &) {
			if (cancel) {
				throw std::runtime_error("Native LSFG preparation cancelled");
			}
			// Let foreground PSP shader compilation acquire UAM between modules.
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		});
		if (cancel) {
			result.cancelled = true;
			return result;
		}
		if (!lsfg::succeeded(status)) {
			result.error = "Native LSFG shader preparation failed: " + std::string(lsfg::error_name(status));
			return result;
		}
		std::error_code filesystemError;
		std::filesystem::create_directories(result.directory, filesystemError);
		if (filesystemError || !lsfg::succeeded(lsfg::cache::write(result.directory, prepared.contents)) ||
			!lsfg::succeeded(lsfg::cache::read(result.directory, cached)) || !lsfg::cache::same_modules(cached, prepared.contents)) {
			result.error = "Native LSFG shader cache could not be saved and verified";
		}
	} catch (const std::exception &error) {
		result.cancelled = cancel;
		if (!result.cancelled) {
			result.error = error.what();
		}
	}
	return result;
}

} // namespace Deko3DLSFG
