// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Common/File/Path.h"
#include "Common/UI/PopupScreens.h"

class GameInfo;

class SwitchForwarderScreen : public UI::PopupScreen {
public:
	SwitchForwarderScreen(const Path &gamePath, std::string title);
	~SwitchForwarderScreen() override;

	const char *tag() const override { return "SwitchForwarder"; }

protected:
	void CreatePopupContents(UI::ViewGroup *parent) override;
	void update() override;
	bool ShowButtons() const override { return false; }
	UI::Size PopupWidth() const override { return 720; }
	bool CanComplete(DialogResult result) override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;

	void StartSteamGridDB();
	void ChooseLocalImage();
	void UseGameIcon();
	void CreateForwarder();
	bool ApplyIcon(std::vector<uint8_t> data, std::string source);
	void ShowError(std::string message);

	Path gamePath_;
	std::string name_;
	std::string searchQuery_;
	std::string author_ = "naga";
	std::string iconSource_ = "Loading game icon...";
	std::vector<uint8_t> gameIcon_;
	std::vector<uint8_t> iconData_;
	std::shared_ptr<GameInfo> gameInfo_;
	bool checkedGameIcon_ = false;
};
