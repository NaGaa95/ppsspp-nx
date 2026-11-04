// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "Common/Data/Format/JSONReader.h"

struct SwitchReleaseInfo {
	std::string tag;
	std::string name;
	std::string notes;
	std::string assetName;
	std::string assetUrl;
	std::string assetDigest;
	uint64_t assetSize = 0;
};

inline bool SwitchUpdate_ParseRelease(std::string_view data, SwitchReleaseInfo *release, std::string *error) {
	json::JsonReader reader(data.data(), data.size());
	json::JsonGet root = reader.root();
	SwitchReleaseInfo found;
	if (!reader.ok() || !root || !root.getString("tag_name", &found.tag) || found.tag.empty()) {
		*error = "GitHub returned a release without a valid version.";
		return false;
	}
	root.getString("name", &found.name);
	root.getString("body", &found.notes);
	if (found.name.empty()) {
		found.name = found.tag;
	}
	if (found.notes.empty()) {
		found.notes = "No changelog was provided for this release.";
	}
	const JsonNode *assets = root.getArray("assets");
	if (assets) {
		for (const JsonNode *node : assets->value) {
			json::JsonGet asset = node->value;
			std::string name, url;
			const double size = asset.getFloat("size", -1.0);
			if (!asset.getString("name", &name) || !asset.getString("browser_download_url", &url) || !std::isfinite(size) ||
				size < 1024 * 1024 || size > 512ULL * 1024 * 1024 || std::floor(size) != size) {
				continue;
			}
			std::string lowerName = name;
			std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) { return (char)std::tolower(c); });
			constexpr std::string_view prefix = "https://github.com/NaGaa95/ppsspp-nx/releases/download/";
			if (lowerName != "ppsspp.nro" || url.compare(0, prefix.size(), prefix) != 0) {
				continue;
			}
			found.assetName = std::move(name);
			found.assetUrl = std::move(url);
			found.assetSize = (uint64_t)size;
			asset.getString("digest", &found.assetDigest);
			*release = std::move(found);
			return true;
		}
	}
	*error = "The latest release does not contain PPSSPP.nro.";
	return false;
}
