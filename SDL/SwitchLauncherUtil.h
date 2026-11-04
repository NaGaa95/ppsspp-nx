// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace SwitchLauncher {

struct MenuRowsLayout {
	int top, height, visible;
};

inline MenuRowsLayout LayoutMenuRows(int count, int top, int bottom, int gap, bool fit) {
	const int available = std::max(1, bottom - top - gap);
	const int height = fit ? std::clamp(available / std::max(1, count), 32, 44) : 44;
	const int visible = std::min(count, std::max(1, available / height));
	return {top + (fit ? std::max(0, (available - visible * height) / 2) : 0), height, visible};
}

inline int ArtworkUploadRows(int width, int height, int nextRow, size_t budget) {
	if (width <= 0 || height <= 0 || nextRow < 0 || nextRow >= height) {
		return 0;
	}
	const size_t rows = std::max<size_t>(1, budget / ((size_t)width * 4));
	return (int)std::min<size_t>(height - nextRow, rows);
}

inline std::string NormalizeLibraryPath(std::string path) {
	std::replace(path.begin(), path.end(), '\\', '/');
	for (char &c : path) {
		if (c >= 'A' && c <= 'Z') {
			c += 'a' - 'A';
		}
	}
	if (path.rfind("sdmc:/", 0) == 0) {
		path.erase(0, 5);
	}
	while (path.size() > 1 && path.back() == '/') {
		path.pop_back();
	}
	constexpr std::string_view eboot = "/eboot.pbp";
	if (path.size() > eboot.size() && path.compare(path.size() - eboot.size(), eboot.size(), eboot) == 0) {
		path.resize(path.size() - eboot.size());
	}
	return path;
}

inline int LibraryPage(int selection, int direction, int columns, int rows, int count, bool recent) {
	if (count <= 0) {
		return 0;
	}
	selection = std::clamp(selection, 0, count - 1);
	if (recent) {
		return std::clamp(selection + direction * columns, 0, count - 1);
	}
	const int perPage = columns * rows;
	const int page = std::clamp(selection / perPage + direction, 0, (count - 1) / perPage);
	return std::min(page * perPage + selection % perPage, count - 1);
}

inline int LibraryMove(int selection, int direction, int columns, int rows, int count) {
	if (count <= 0 || columns <= 0 || rows <= 0) {
		return 0;
	}
	selection = std::clamp(selection, 0, count - 1);
	if (std::abs(direction) == 1) {
		return std::clamp(selection + direction, 0, count - 1);
	}
	const int pageStart = selection / (columns * rows) * columns * rows;
	const int pageEnd = std::min(pageStart + columns * rows, count);
	const int next = selection + (direction > 0 ? columns : -columns);
	return !direction || next < pageStart || next >= pageEnd ? selection : next;
}

inline bool TriggerPressed(int value, bool &previous) {
	const bool pressed = value > 18000;
	const bool rising = pressed && !previous;
	previous = pressed;
	return rising;
}

// A fixed repeat rate with hysteresis; axis event frequency never changes it.
class StickNavigation {
public:
	void Axis(bool horizontal, int value) { (horizontal ? x_ : y_) = value; }
	void Reset() { x_ = y_ = direction_ = 0; }
	int Next(uint64_t now, bool horizontal) {
		const bool useX = horizontal && std::abs(x_) > std::abs(y_);
		const int value = useX ? x_ : y_;
		const int candidate = (value > 0 ? 1 : -1) * (useX ? 1 : 2);
		const int threshold = candidate == direction_ ? 11000 : 22000;
		const int direction = std::abs(value) > threshold ? candidate : 0;
		if (direction != direction_) {
			direction_ = direction;
			next_ = now + 400;
			return direction;
		}
		if (direction && now >= next_) {
			next_ = now + 170;
			return direction;
		}
		return 0;
	}

private:
	int x_ = 0, y_ = 0, direction_ = 0;
	uint64_t next_ = 0;
};

inline std::string WithoutMnemonics(std::string_view text) {
	std::string result;
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '&' && i + 1 < text.size()) {
			if (text[i + 1] == '&') {
				++i;
			} else if (text[i + 1] != ' ' && text[i + 1] != '\n') {
				continue;
			}
		}
		result += text[i];
	}
	return result;
}

inline float SliderValue(float value, float minimum, float maximum, float step) {
	value = std::clamp(value, minimum, maximum);
	if (value == minimum || value == maximum) {
		return value;
	}
	return std::clamp(minimum + std::round((value - minimum) / step) * step, minimum, maximum);
}

// Only settings with a working Switch consumer belong in the SDL frontend.
// Their order is independent of the cross-platform configuration file layout.
inline const std::vector<std::string_view> &LauncherSettingKeys(std::string_view section) {
	static const std::vector<std::string_view> general{
		"AutoLoadSaveState", "SaveStateSlotCount", "EnableStateUndo", "ConfirmLoadState", "RewindSnapshotInterval",
		"ScreenshotMode",	 "ScreenshotsAsPNG",   "EnableCheats",	  "EnablePlugins",	  "ReportAccurateFreeStorageSpace",
	};
	static const std::vector<std::string_view> cpu{
		"CPUCore",
		"CPUSpeed",
		"FastMemoryAccess",
		"IOTimingMethod",
	};
	static const std::vector<std::string_view> graphics{
		"GraphicsBackend", "InternalResolution", "MultiSampleLevel",	  "VerticalSync",	  "LowLatencyPresent",	 "InflightFrames",
		"FrameSkip",	   "AutoFrameSkip",		 "RenderDuplicateFrames", "TextureFiltering", "Smart2DTexFiltering", "AnisotropyLevel",
	};
	static const std::vector<std::string_view> textures{
		"TexScalingLevel", "TexScalingType", "TexDeposterize", "ReplaceTextures", "ReplacementTextureLoadSpeed",
	};
	static const std::vector<std::string_view> performance{
		"SkipBufferEffects",
		"SkipGPUReadbackMode",
		"BloomHack",
		"DepthRasterMode",
	};
	static const std::vector<std::string_view> speed{
		"AltSpeedPercent",
		"AltSpeedPercent2",
		"AnalogAltSpeedPercent",
	};
	static const std::vector<std::string_view> audio{
		"Enable", "GameVolume", "ReverbRelativeVolume", "AltSpeedRelativeVolume", "AchievementVolume", "AudioSyncMode", "FillAudioGaps",
	};
	static const std::vector<std::string_view> system{
		"PSPModel", "NickName", "GameLanguage", "ButtonPreference", "ParamTimeFormat", "ParamDateFormat", "DayLightSavings", "MemStickSize",
	};
	static const std::vector<std::string_view> controls{
		"AnalogLimiterDeadzone", "AnalogTriggerThreshold", "AnalogStickThreshold", "RapidFileInterval",
		"ShowTouchControls",	 "TouchButtonStyle",	   "TouchButtonOpacity",   "TouchButtonHideSeconds",
		"DisableDpadDiagonals",	 "AutoCenterTouchAnalog",  "StickyTouchDPad",
	};
	static const std::vector<std::string_view> network{
		"EnableWlan",
		"EnableAdhocServer",
		"proAdhocServer",
		"AdhocServerRelayMode",
		"AdhocUseUdp",
		"AdhocRelayModeEnetPdpReliable",
		"PortOffset",
		"InfrastructureUsername",
		"InfrastructureAutoDNS",
		"PrimaryDNSServer",
		"MinTimeout",
		"ForcedFirstConnect",
		"EnableNetworkChat",
		"ChatButtonPosition",
		"ChatScreenPosition",
		"ChatTimestamps",
		"EnableQuickChat",
		"QuickChat1",
		"QuickChat2",
		"QuickChat3",
		"QuickChat4",
		"QuickChat5",
	};
	static const std::vector<std::string_view> empty;
	if (section == "General") {
		return general;
	}
	if (section == "CPU") {
		return cpu;
	}
	if (section == "Graphics") {
		return graphics;
	}
	if (section == "Textures") {
		return textures;
	}
	if (section == "Performance") {
		return performance;
	}
	if (section == "Speed") {
		return speed;
	}
	if (section == "Sound") {
		return audio;
	}
	if (section == "SystemParam") {
		return system;
	}
	if (section == "Control") {
		return controls;
	}
	if (section == "Network") {
		return network;
	}
	return empty;
}

} // namespace SwitchLauncher
