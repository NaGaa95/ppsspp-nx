// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

#include <memory>
#include <string>

#include "UI/SwitchUpdateRelease.h"

void SwitchUpdate_SetExecutablePath(std::string path);
bool SwitchUpdate_RecoverInstallation(std::string *error);

enum class SwitchUpdateState {
	Checking,
	UpdateAvailable,
	UpToDate,
	Downloading,
	ReadyToInstall,
	Installing,
	Installed,
	Cancelled,
	Error,
};

struct SwitchUpdateSnapshot {
	SwitchUpdateState state = SwitchUpdateState::Checking;
	SwitchReleaseInfo release;
	std::string error;
	uint64_t downloaded = 0;
	uint64_t total = 0;
};

class SwitchUpdateTask {
public:
	SwitchUpdateTask();
	~SwitchUpdateTask();
	SwitchUpdateSnapshot Snapshot();
	void StartCheck();
	void StartDownload();
	void Cancel();
	bool Install();
private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};
