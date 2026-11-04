// Copyright (c) 2012- PPSSPP Project.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

#pragma once

class TextDrawer;
namespace Draw {
class DrawContext;
}

// Uses the launcher's font/shaping engine with the running game's draw context.
TextDrawer *CreateSwitchTextDrawer(Draw::DrawContext *draw);
