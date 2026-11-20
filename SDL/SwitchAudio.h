// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

// Plays NativeMix through Horizon's audout service.
namespace SwitchAudio {

constexpr int kSampleRate = 48000;
constexpr int kPeriodFrames = 512;

bool Start();
void Stop();

}  // namespace SwitchAudio
