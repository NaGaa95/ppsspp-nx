// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <memory>
#include <string>

#include "Common/UI/PopupScreens.h"

namespace UI {
class Choice;
class ProgressBar;
class ScrollView;
class TextView;
class ViewGroup;
}

void SwitchUpdate_SetExecutablePath(std::string path);
bool SwitchUpdate_RecoverInstallation(std::string *error);

class SwitchUpdateScreen : public UI::PopupScreen {
public:
	SwitchUpdateScreen();
	~SwitchUpdateScreen() override;

	const char *tag() const override { return "SwitchUpdate"; }

protected:
	void CreatePopupContents(UI::ViewGroup *parent) override;
	void update() override;
	bool ShowButtons() const override { return false; }
	UI::Size PopupWidth() const override { return 700; }
	bool CanComplete(DialogResult result) override;
	void OnCompleted(DialogResult result) override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;

	void Refresh();
	void OnAction(UI::EventParams &e);
	void OnClose(UI::EventParams &e);

	UI::TextView *statusView_ = nullptr;
	UI::TextView *releaseView_ = nullptr;
	UI::TextView *notesView_ = nullptr;
	UI::ScrollView *notesScroll_ = nullptr;
	UI::ProgressBar *progressBar_ = nullptr;
	UI::Choice *actionChoice_ = nullptr;
	UI::Choice *closeChoice_ = nullptr;
	bool restartRequested_ = false;
};
